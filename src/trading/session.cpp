#include "openport/trading/session.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

#include "state.hpp"

namespace openport::trading {
namespace {
using detail::State;
using Events = std::vector<Json>;
Decision failure(Reason code, std::string message) { return {code, std::move(message), {}, {}, {}}; }
void event(Events& events, std::string_view type, Json payload) {
  events.push_back(Json{{"type", type}, {"payload", std::move(payload)}});
}
md::Date local_date(Timestamp time) {
  // At regular trading hours UTC and New York share the same date; this also
  // handles explicit overnight rollover commands using the correct DST offset.
  auto date = md::date_from_days(time / md::kNanosPerDay);
  const auto offset = md::new_york_utc_offset_hours(date, 0);
  auto seconds = time / md::kNanosPerSecond + offset * 3600;
  auto days = seconds / 86400;
  if (seconds < 0 && seconds % 86400 != 0) --days;
  return md::date_from_days(days);
}
Timestamp regular_end(const md::OptionContract& c, Timestamp time) {
  const auto date = local_date(time);
  // Index and SPY/QQQ/IWM/DIA options trade through :15 past the close; other
  // equity options stop on the hour.
  for (const int minute : {15, 0}) {
    const auto end = md::new_york_to_utc(date, md::regular_close_hour(date), minute);
    if (end != md::kInvalidTimestamp && md::trading_session(c.root, end - 1).name == "regular" &&
        md::trading_session(c.root, end).name != "regular")
      return end;
  }
  throw TradingError(Reason::SESSION_CLOSED, "No representable regular session end");
}
Money mid(const QuoteObservation& q) {
  // Both sides are positive. Difference-first avoids overflowing their sum.
  return *q.bid + (*q.ask - *q.bid).prorate(1, 2);
}
Decision quote_check(const State& s, const std::string& symbol) {
  const auto it = s.books.find(symbol);
  if (it == s.books.end() || !valid_quote(it->second.quote))
    return failure(Reason::INVALID_QUOTE, "A noncrossed positive two-sided quote with both sizes is required");
  const auto time = it->second.quote.time;
  if (time > s.time || s.time - time > s.config.limits.max_quote_age)
    return failure(Reason::STALE_QUOTE, "Quote is outside the configured market-time freshness window");
  return {};
}
Decision price_check(const State& s, const QuoteObservation& q, Money price) {
  const auto middle = mid(q);
  const long double difference = std::abs(static_cast<long double>(price.micros()) - static_cast<long double>(middle.micros()));
  const long double band = std::max(static_cast<long double>(s.config.limits.price_band_absolute.micros()),
      static_cast<long double>(s.config.limits.price_band_relative) * static_cast<long double>(middle.micros()));
  if (difference > band)
    return {Reason::PRICE_BAND, "Price is outside the configured band around mid",
            static_cast<double>(difference / 1'000'000), static_cast<double>(band / 1'000'000), q.symbol};
  return {};
}
Quantity magnitude(Quantity q) { return q < 0 ? -q : q; }
Quantity held(const State& s, const std::string& symbol) {
  const auto it = s.ledger.positions().find(symbol);
  return it == s.ledger.positions().end() ? 0 : it->second.quantity;
}
std::optional<double> spot_for(const State& s, const std::string& symbol) {
  const auto it = s.valuations.find(symbol);
  if (it == s.valuations.end() || !valid_valuation(it->second)) return std::nullopt;
  return it->second.spot;
}
/// A bracket's two exits can fill only once between them: checks count the pair
/// through its earlier order, so the later one is shadowed while both are open.
bool shadowed(const State& s, const Order& o) {
  return o.oco != 0 && o.oco < o.id && s.orders.at(static_cast<std::size_t>(o.oco - 1)).open();
}
/// Open ordinary user orders on one side of a contract, excluding `self`. Bracket
/// exits are left out: they shrink with the position, so they never oversell and
/// never block a manual close.
Quantity pending(const State& s, const std::string& symbol, Side side, const Order& self) {
  Quantity total = 0;
  for (const auto& o : s.orders)
    if (o.open() && !o.system && o.role == OrderRole::Normal && o.id != self.id &&
        o.request.symbol == symbol && o.request.side == side)
      total += o.remaining();
  return total;
}
/// True when this order, together with the other working orders on its side,
/// can only reduce the current position toward flat.
bool closing_only(const State& s, const Order& o) {
  const auto q = held(s, o.request.symbol);
  const auto side = o.request.side;
  const auto others = pending(s, o.request.symbol, side, o);
  return side == Side::Sell ? q > 0 && o.remaining() + others <= q
                            : q < 0 && o.remaining() + others <= -q;
}
Money average_unit_price(const Position& p) {
  const auto size = magnitude(p.quantity);
  if (size == 0) return {};
  return (p.basis < Money{} ? -p.basis : p.basis).prorate(1, size * 100);
}
struct PowerDetail {
  BuyingPower total;
  Money focus_reservation;
  Quantity focus_opening = 0;
};
/// Working orders reserve in acceptance order. Closing capacity is consumed by
/// earlier orders first, so two sells cannot both claim the same long contracts.
/// Opening buys reserve premium plus fees; opening sells reserve the naked
/// requirement plus fees (their credit covers the buy-back value); closing
/// orders reserve only fees. Orders without a limit use the current far side.
PowerDetail buying_power(const State& s, OrderId focus = 0) {
  PowerDetail out;
  Money short_requirement;
  for (const auto& [symbol, position] : s.ledger.positions()) {
    if (position.quantity >= 0) continue;
    const auto size = -position.quantity;
    const auto mark = s.marks.find(symbol);
    // Without a mark the entry credit stands in for the buy-back value.
    const Money value = mark != s.marks.end() ? (mark->second.price * 100) * size : -position.basis;
    short_requirement = short_requirement + value + naked_requirement(position.contract, spot_for(s, symbol)) * size;
  }
  std::map<std::string, std::pair<Quantity, Quantity>> capacity;
  Money reserved;
  for (const auto& o : s.orders) {
    if (!o.open() || o.remaining() <= 0 || shadowed(s, o)) continue;
    const auto& symbol = o.request.symbol;
    const auto contract = s.contracts.find(symbol);
    if (contract == s.contracts.end()) continue;
    const auto q = held(s, symbol);
    auto& [long_left, short_left] = capacity.try_emplace(symbol, std::max<Quantity>(q, 0), std::max<Quantity>(-q, 0)).first->second;
    const auto remaining = o.remaining();
    Money price;
    if (o.request.limit_price) price = *o.request.limit_price;
    else if (const auto book = s.books.find(symbol); book != s.books.end() && valid_quote(book->second.quote))
      price = o.request.side == Side::Buy ? *book->second.quote.ask : *book->second.quote.bid;
    Money reservation = s.config.fee_per_contract * remaining;
    Quantity opening = 0;
    if (o.role != OrderRole::Normal) {
      // Bracket exits stay within the position, so they only ever close; they
      // leave closing capacity to ordinary orders such as a manual close.
    } else if (o.request.side == Side::Buy) {
      const auto closing = std::min(remaining, short_left);
      short_left -= closing;
      opening = remaining - closing;
      reservation = reservation + (price * 100) * opening;
    } else {
      const auto closing = std::min(remaining, long_left);
      long_left -= closing;
      opening = remaining - closing;
      reservation = reservation + naked_requirement(contract->second, spot_for(s, symbol)) * opening;
    }
    reserved = reserved + reservation;
    if (o.id == focus) { out.focus_reservation = reservation; out.focus_opening = opening; }
  }
  out.total.reserved = reserved;
  out.total.short_requirement = short_requirement;
  out.total.available = s.ledger.account().cash - short_requirement - reserved;
  return out;
}
TradingSnapshot snapshot_of(const State& s) {
  TradingSnapshot out;
  out.account_version = s.version;
  out.time = s.time;
  out.account = s.ledger.account();
  out.equity = out.account.cash;
  out.start_of_day_equity = s.start_equity;
  out.recent_orders = s.orders;
  out.recent_fills = s.fills;
  for (const auto& order : s.orders) if (order.open()) out.open_orders.push_back(order);
  for (const auto& [symbol, position] : s.ledger.positions()) {
    MarkedPosition p;
    p.position = position;
    p.awaiting_settlement = s.time >= position.contract.expiry_time();
    const auto it = s.marks.find(symbol);
    if (it != s.marks.end()) {
      p.mark = it->second.price;
      p.mark_time = it->second.time;
      p.mark_age = s.time - p.mark_time;
      p.market_value = (*p.mark * 100) * position.quantity;
      p.unrealised = *p.market_value - position.basis;
      p.fresh = !p.awaiting_settlement && quote_check(s, symbol).ok();
      out.equity = out.equity + *p.market_value;
      out.unrealised = out.unrealised + *p.unrealised;
    }
    out.valuation_complete &= p.fresh;
    out.positions.push_back(std::move(p));
  }
  if (std::any_of(s.orders.begin(), s.orders.end(), [&](const Order& o) { return shadowed(s, o); })) {
    // Reachable exposure counts each open bracket pair once.
    auto orders = s.orders;
    for (auto& o : orders) if (shadowed(s, o)) o.status = OrderStatus::Cancelled;
    out.risk = portfolio_risk(s.ledger, orders, s.contracts, s.valuations, s.config.limits, s.time);
  } else {
    out.risk = portfolio_risk(s.ledger, s.orders, s.contracts, s.valuations, s.config.limits, s.time);
  }
  out.risk.daily_loss = std::max(Money{}, s.start_equity - out.equity);
  out.risk.kill_latched = s.kill;
  out.risk.kill_reason = s.kill_reason;
  out.risk.limits_revision = s.limits_revision;
  out.scenarios = scenario_grid(s.ledger, s.valuations, s.config.scenarios, s.time, s.config.limits.max_valuation_age);
  if (!out.valuation_complete) out.quality_flags.push_back(Reason::STALE_QUOTE);
  if (!out.risk.complete || !out.scenarios.complete) out.quality_flags.push_back(Reason::MISSING_VALUATION);
  if (std::any_of(out.positions.begin(), out.positions.end(), [](const auto& p) { return p.awaiting_settlement; }))
    out.quality_flags.push_back(Reason::AWAITING_SETTLEMENT);
  out.evaluation = s.evaluation;
  out.buying_power = buying_power(s).total;
  out.closures = s.closures;
  out.attempts = s.attempts;
  return out;
}
/// Rules only act on fully marked equity: every position has a mark, fresh or not.
std::optional<Money> marked_equity(const TradingSnapshot& snapshot) {
  for (const auto& p : snapshot.positions) if (!p.market_value) return std::nullopt;
  return snapshot.equity;
}
/// The trailing floor, peak - max drawdown. With a lock balance it stops once
/// it reaches that level and stays there (`locked` latches).
Money floor_for(const AccountRules& rules, Money peak, bool& locked) {
  if (rules.max_drawdown <= Money{}) return {};
  if (rules.lock_balance > Money{} && (locked || peak - rules.max_drawdown >= rules.lock_balance)) {
    locked = true;
    return rules.lock_balance;
  }
  return peak - rules.max_drawdown;
}
Money net_realised(const State& s) { return s.ledger.account().realised - s.ledger.account().fees; }
Money whole_cents(Money value) {
  return value > Money{} ? Money::from_micros(value.micros() / 10'000 * 10'000) : Money{};
}
std::string dollars(Money value) { return (value < Money{} ? "-$" + (-value).str() : "$" + value.str()); }
Evaluation fresh_evaluation(const State& s, std::uint64_t attempt) {
  Evaluation e;
  e.attempt = attempt;
  e.started = s.time;
  e.starting_balance = s.ledger.account().cash;
  e.peak = e.starting_balance;
  e.floor = floor_for(s.config.rules, e.peak, e.floor_locked);
  e.day_open_realised = net_realised(s);
  e.cycle_started = s.time;
  e.first_order = static_cast<OrderId>(s.orders.size() + 1);
  e.first_fill = s.fills.size() + 1;
  e.day = s.day;
  e.day_open_equity = e.starting_balance;
  e.day_close_equity = e.starting_balance;
  return e;
}
void cancel_order(Order& order, Decision reason, Events& events) {
  if (!order.open()) return;
  order.status = OrderStatus::Cancelled;
  order.reason = std::move(reason);
  event(events, "cancel", order);
}
void trip(State& s, const std::string& reason, Events& events) {
  if (!s.kill) {
    s.kill = true;
    s.kill_reason = reason;
    event(events, "kill_trip", Json{{"reason", reason}});
  }
  for (auto& o : s.orders) cancel_order(o, failure(Reason::KILL_SWITCH, s.kill_reason), events);
}
Decision loss_check(const State& s, const TradingSnapshot& snapshot) {
  if (snapshot.risk.daily_loss > s.config.limits.max_daily_loss)
    return {Reason::DAILY_LOSS, "Marked loss from start-of-day equity exceeds limit",
        snapshot.risk.daily_loss.dollars(), s.config.limits.max_daily_loss.dollars(), "aggregate"};
  return {};
}
void monitor_loss(State& s, Events& events) {
  const auto snapshot = snapshot_of(s);
  if (!loss_check(s, snapshot).ok()) trip(s, "DAILY_LOSS", events);
}
void advance(State& s, Timestamp time, Events& events) {
  if (time < 0 || time < s.time) throw TradingError(Reason::INVALID_TIME, "Market time must be nonnegative and monotone");
  s.time = time;
  for (auto& o : s.orders) {
    if (!o.open()) continue;
    const auto& contract = s.contracts.at(o.request.symbol);
    if (time >= contract.expiry_time()) cancel_order(o, failure(Reason::EXPIRED, "Contract reached expiry and awaits settlement"), events);
    else if (time >= o.day_end) cancel_order(o, failure(Reason::DAY_END, "Regular session ended"), events);
  }
}
Decision order_check(const State& s, const Order& o, bool at_fill = false) {
  if (s.kill) return failure(Reason::KILL_SWITCH, s.kill_reason);
  const auto& rules = s.config.rules;
  if (rules.evaluation() && s.evaluation.status != EvaluationStatus::Active)
    return failure(Reason::EVALUATION_CLOSED, std::string("The evaluation has ") +
        (s.evaluation.status == EvaluationStatus::Passed ? "passed" : "failed") +
        "; reset the account to start a new attempt");
  const auto& request = o.request;
  const auto c = s.contracts.find(request.symbol);
  if (c == s.contracts.end()) return failure(Reason::UNKNOWN_CONTRACT, "Order must reference a registered canonical OSI definition");
  if (s.time >= c->second.expiry_time()) return failure(Reason::EXPIRED, "Contract has expired");
  if (md::trading_session(c->second.root, s.time).name != "regular")
    return failure(Reason::SESSION_CLOSED, "v1 accepts and executes only in the product regular session");
  if (request.client_order_id.empty() || request.quantity <= 0 ||
      (request.side != Side::Buy && request.side != Side::Sell) ||
      (request.type != OrderType::Market && request.type != OrderType::Limit) ||
      (request.tif != TimeInForce::Day && request.tif != TimeInForce::Ioc) ||
      (request.type == OrderType::Market && (request.tif != TimeInForce::Ioc || request.limit_price)) ||
      (request.type == OrderType::Limit && (!request.limit_price || *request.limit_price <= Money{})))
    return failure(Reason::INVALID_ORDER, "Positive quantity/client ID required; market is IOC without price; limit requires positive price");
  if (request.quantity > s.config.limits.max_order_contracts)
    return {Reason::MAX_ORDER_CONTRACTS, "Order contract count exceeds limit", static_cast<double>(request.quantity),
            static_cast<double>(s.config.limits.max_order_contracts), request.symbol};
  if (request.limit_price && request.limit_price->micros() % tick_size(c->second.root, *request.limit_price).micros() != 0)
    return failure(Reason::INVALID_TICK, "Limit price is not a positive multiple of the product tier tick");
  const auto positive = [](const std::optional<Trigger>& t) { return !t || t->level > Money{}; };
  const auto exit_ok = [&](const std::optional<ExitSpec>& e) {
    return !e || (e->trigger.has_value() != e->limit_price.has_value() && positive(e->trigger) &&
                  (!e->limit_price || *e->limit_price > Money{}));
  };
  const auto& bracket = request.bracket;
  if (!positive(request.trigger) ||
      (bracket && (!exit_ok(bracket->stop_loss) || !exit_ok(bracket->take_profit) || (!bracket->stop_loss && !bracket->take_profit))))
    return failure(Reason::INVALID_ORDER, "Trigger levels and exit prices must be positive; each exit takes either a trigger or a limit price");
  if (bracket && bracket->take_profit && bracket->take_profit->limit_price &&
      bracket->take_profit->limit_price->micros() % tick_size(c->second.root, *bracket->take_profit->limit_price).micros() != 0)
    return failure(Reason::INVALID_TICK, "Take-profit price is not a positive multiple of the product tier tick");
  if (rules.buy_only && request.side == Side::Sell && !closing_only(s, o))
    return failure(Reason::BUY_ONLY, "This plan is buy-only: sells may only close contracts you already hold");
  if (rules.expiry_cutoff > 0 && s.time >= c->second.expiry_time() - rules.expiry_cutoff && !closing_only(s, o))
    return failure(Reason::EXPIRY_CUTOFF, "Contract is inside the pre-expiry cutoff; only closing orders are accepted");
  if (const auto d = quote_check(s, request.symbol); !d.ok()) return d;
  const auto& quote = s.books.at(request.symbol).quote;
  const auto price = !at_fill && request.limit_price ? *request.limit_price : (request.side == Side::Buy ? *quote.ask : *quote.bid);
  if (const auto d = price_check(s, quote, price); !d.ok()) return d;
  const auto snapshot = snapshot_of(s);
  if (!snapshot.valuation_complete) return failure(Reason::STALE_QUOTE, "All held positions need fresh marks before trading");
  if (const auto d = loss_check(s, snapshot); !d.ok()) return d;
  if (const auto d = check_exposure(snapshot.risk); !d.ok()) return d;
  if (rules.buying_power && !at_fill) {
    // Fills recheck buying power against the projected ledger instead.
    const auto power = buying_power(s, o.id);
    if (power.focus_opening > 0 && power.total.available < Money{})
      return {Reason::BUYING_POWER, "Order needs more buying power than the account has available",
              power.focus_reservation.dollars(), (power.total.available + power.focus_reservation).dollars(), request.symbol};
  }
  return {};
}
/// Liquidation and expiry auto-close reduce risk, so only the contract,
/// session and executable-quote gates apply, including under the kill latch.
Decision system_check(const State& s, const Order& o) {
  const auto c = s.contracts.find(o.request.symbol);
  if (c == s.contracts.end()) return failure(Reason::UNKNOWN_CONTRACT, "Order must reference a registered canonical OSI definition");
  if (s.time >= c->second.expiry_time()) return failure(Reason::EXPIRED, "Contract has expired");
  if (md::trading_session(c->second.root, s.time).name != "regular")
    return failure(Reason::SESSION_CLOSED, "v1 accepts and executes only in the product regular session");
  return quote_check(s, o.request.symbol);
}
bool opens(Quantity held_quantity, Quantity signed_fill) {
  return held_quantity == 0 || (held_quantity > 0) == (signed_fill > 0) || magnitude(signed_fill) > magnitude(held_quantity);
}
bool marketable(const Order& o, const QuoteObservation& q) {
  if (o.request.type == OrderType::Market) return true;
  return o.request.side == Side::Buy ? *q.ask <= *o.request.limit_price : *q.bid >= *o.request.limit_price;
}
void on_fill(State& s, OrderId id, Events& events);
void match_one(State& s, OrderId id, Events& events, std::optional<OrderId> incoming) {
  auto& o = s.orders.at(static_cast<std::size_t>(id - 1));
  if (!o.open() || o.status == OrderStatus::Armed) return;
  if (incoming != id && s.books.at(o.request.symbol).quote.time < o.accepted_at) return;
  // Good-until-expiry exits outlive a session; they wait for the next one.
  if (md::trading_session(s.contracts.at(o.request.symbol).root, s.time).name != "regular" && o.role != OrderRole::Normal) return;
  if (!quote_check(s, o.request.symbol).ok() || !marketable(o, s.books.at(o.request.symbol).quote)) return;
  // System orders and bracket exits only ever reduce a position (exits are kept
  // within it), so they skip the price band and loss projection when executing.
  const bool reducing = o.system || o.role != OrderRole::Normal;
  auto decision = reducing ? system_check(s, o) : order_check(s, o, true);
  if (!decision.ok()) {
    decision.message = std::string(to_string(decision.code)) + ": " + decision.message;
    decision.code = Reason::RISK_CHANGED;
    cancel_order(o, decision, events);
    return;
  }
  auto& book = s.books.at(o.request.symbol);
  if (!marketable(o, book.quote)) return;
  const Money price = o.request.side == Side::Buy ? *book.quote.ask : *book.quote.bid;
  auto& budget = o.request.side == Side::Buy ? book.ask_left : book.bid_left;
  const Quantity quantity = std::min(o.remaining(), budget);
  if (quantity <= 0) return;
  decision = reducing ? Decision{} : price_check(s, book.quote, price);
  const Money fee = s.config.fee_per_contract * quantity;
  if (decision.ok() && !reducing) {
    // Check the proposed accounting before committing any liquidity or fill.
    const Quantity signed_quantity = o.request.side == Side::Buy ? quantity : -quantity;
    const auto before = held(s, o.request.symbol);
    State projected = s;
    projected.ledger.fill(s.contracts.at(o.request.symbol), signed_quantity, price, fee);
    projected.orders.at(static_cast<std::size_t>(id - 1)).filled_quantity += quantity;
    decision = loss_check(projected, snapshot_of(projected));
    if (decision.ok() && s.config.rules.buying_power && opens(before, signed_quantity)) {
      const auto power = buying_power(projected).total;
      if (power.available < Money{})
        decision = {Reason::BUYING_POWER, "Fill needs more buying power than the account has available",
                    (-power.available).dollars(), 0.0, o.request.symbol};
    }
  }
  if (!decision.ok()) {
    decision.message = std::string(to_string(decision.code)) + ": " + decision.message;
    decision.code = Reason::RISK_CHANGED;
    cancel_order(o, decision, events);
    return;
  }
  s.ledger.fill(s.contracts.at(o.request.symbol), o.request.side == Side::Buy ? quantity : -quantity, price, fee);
  budget -= quantity;
  o.filled_quantity += quantity;
  o.filled_notional = o.filled_notional + price * quantity;
  o.status = o.remaining() == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
  Fill fill{static_cast<std::uint64_t>(s.fills.size() + 1), id, o.request.symbol, o.request.side,
            quantity, price, fee, book.quote.observation, book.quote.time, s.time};
  s.fills.push_back(fill);
  event(events, "fill", fill);
  on_fill(s, id, events);
}
void match_symbols(State& s, const std::set<std::string>& symbols, Events& events,
                   std::optional<OrderId> incoming = {}) {
  for (const auto& symbol : symbols) {
    for (const auto side : {Side::Buy, Side::Sell}) {
      std::vector<OrderId> priority;
      for (const auto& o : s.orders)
        if (o.open() && o.status != OrderStatus::Armed && o.request.symbol == symbol && o.request.side == side)
          priority.push_back(o.id);
      std::stable_sort(priority.begin(), priority.end(), [&](OrderId a, OrderId b) {
        const auto& x = s.orders.at(static_cast<std::size_t>(a - 1));
        const auto& y = s.orders.at(static_cast<std::size_t>(b - 1));
        if (x.request.type != y.request.type) return x.request.type == OrderType::Market;
        if (x.request.limit_price == y.request.limit_price) return a < b;
        return side == Side::Buy ? x.request.limit_price > y.request.limit_price : x.request.limit_price < y.request.limit_price;
      });
      for (auto id : priority) match_one(s, id, events, incoming);
    }
  }
}
/// Keep bracket exits within the position they protect: shrink them when it
/// shrinks and cancel them once it is closed, so an exit can never open one.
void sync_exits(State& s, const std::string& symbol, Events& events) {
  const auto q = held(s, symbol);
  for (auto& o : s.orders) {
    if (!o.open() || o.role == OrderRole::Normal || o.request.symbol != symbol) continue;
    const Quantity capacity = o.request.side == Side::Sell ? std::max<Quantity>(q, 0) : std::max<Quantity>(-q, 0);
    if (capacity == 0) {
      cancel_order(o, failure(Reason::POSITION_CLOSED, "The position this exit protected is closed"), events);
    } else if (o.remaining() > capacity) {
      o.request.quantity = o.filled_quantity + capacity;
      event(events, "order_resized", o);
    }
  }
}
/// Create a bracket's exits on the entry's first fill and grow them with later
/// fills. A stop is armed until reached, a limit take-profit rests; both are good
/// until expiry, sized to the filled quantity, and cancel each other on a fill.
void attach_exits(State& s, OrderId entry_id, Events& events) {
  const auto entry = s.orders.at(static_cast<std::size_t>(entry_id - 1));  // pushes below invalidate references
  const auto& bracket = *entry.request.bracket;
  const auto expiry = s.contracts.at(entry.request.symbol).expiry_time();
  auto make = [&](const ExitSpec& spec, OrderRole role) {
    Order exit;
    exit.id = static_cast<OrderId>(s.orders.size() + 1);
    exit.request = {entry.request.client_order_id + (role == OrderRole::StopLoss ? ":stop" : ":target"),
                    entry.request.symbol, entry.request.side == Side::Buy ? Side::Sell : Side::Buy,
                    spec.trigger ? OrderType::Market : OrderType::Limit, spec.trigger ? TimeInForce::Ioc : TimeInForce::Day,
                    entry.filled_quantity, spec.limit_price, spec.trigger, {}};
    exit.role = role;
    exit.parent = entry.id;
    exit.status = spec.trigger ? OrderStatus::Armed : OrderStatus::Working;
    exit.accepted_at = s.time;
    exit.day_end = expiry;
    s.orders.push_back(exit);
    event(events, "order_accepted", exit);
    return exit.id;
  };
  auto grow = [&](OrderId id) {
    auto& exit = s.orders.at(static_cast<std::size_t>(id - 1));
    if (exit.open() && exit.filled_quantity == 0 && exit.request.quantity < entry.filled_quantity) {
      exit.request.quantity = entry.filled_quantity;
      event(events, "order_resized", exit);
    }
  };
  auto stop = entry.stop_loss, target = entry.take_profit;
  const bool created = (bracket.stop_loss && stop == 0) || (bracket.take_profit && target == 0);
  if (bracket.stop_loss) { if (stop == 0) stop = make(*bracket.stop_loss, OrderRole::StopLoss); else grow(stop); }
  if (bracket.take_profit) { if (target == 0) target = make(*bracket.take_profit, OrderRole::TakeProfit); else grow(target); }
  auto& stored = s.orders.at(static_cast<std::size_t>(entry_id - 1));
  stored.stop_loss = stop;
  stored.take_profit = target;
  if (created && stop != 0 && target != 0) {
    s.orders.at(static_cast<std::size_t>(stop - 1)).oco = target;
    s.orders.at(static_cast<std::size_t>(target - 1)).oco = stop;
  }
}
void on_fill(State& s, OrderId id, Events& events) {
  const auto oco = s.orders.at(static_cast<std::size_t>(id - 1)).oco;
  if (oco != 0) {
    auto& sibling = s.orders.at(static_cast<std::size_t>(oco - 1));
    if (sibling.open()) cancel_order(sibling, failure(Reason::OCO_FILLED, "The other exit of this bracket filled"), events);
  }
  if (s.orders.at(static_cast<std::size_t>(id - 1)).request.bracket) attach_exits(s, id, events);
  sync_exits(s, s.orders.at(static_cast<std::size_t>(id - 1)).request.symbol, events);
}
/// Option triggers read the order's executable side from a fresh book; underlying
/// triggers read spot from a fresh valuation. Missing data never triggers.
bool reached(const State& s, const Order& o) {
  const auto& t = *o.request.trigger;
  std::optional<Money> value;
  if (t.source == TriggerSource::Underlying) {
    const auto it = s.valuations.find(o.request.symbol);
    if (it != s.valuations.end() && valid_valuation(it->second) && it->second.time <= s.time &&
        s.time - it->second.time <= s.config.limits.max_valuation_age) {
      try { value = Money::from_double(it->second.spot); } catch (const TradingError&) {}
    }
  } else if (quote_check(s, o.request.symbol).ok()) {
    const auto& q = s.books.at(o.request.symbol).quote;
    value = o.request.side == Side::Buy ? *q.ask : *q.bid;
  }
  return value && (t.direction == TriggerDirection::AtOrBelow ? *value <= t.level : *value >= t.level);
}
/// A reached order runs its checks now: exits need only an executable book;
/// entries take every pre-trade check. Stale data or a closed session keeps it
/// armed for a later batch; any other failure cancels it with RISK_CHANGED.
void activate(State& s, OrderId id, Events& events) {
  {
    auto& o = s.orders.at(static_cast<std::size_t>(id - 1));
    auto d = o.role != OrderRole::Normal ? system_check(s, o) : order_check(s, o);
    if (d.code == Reason::STALE_QUOTE || d.code == Reason::INVALID_QUOTE || d.code == Reason::MISSING_VALUATION ||
        d.code == Reason::SESSION_CLOSED)
      return;
    if (!d.ok()) {
      d.message = std::string(to_string(d.code)) + ": " + d.message;
      d.code = Reason::RISK_CHANGED;
      cancel_order(o, d, events);
      return;
    }
    o.status = OrderStatus::Working;
    o.triggered_at = s.time;
    event(events, "order_triggered", o);
  }
  match_symbols(s, {s.orders.at(static_cast<std::size_t>(id - 1)).request.symbol}, events, id);
  auto& stored = s.orders.at(static_cast<std::size_t>(id - 1));
  if (stored.open() && stored.request.tif == TimeInForce::Ioc)
    cancel_order(stored, failure(Reason::IOC_REMAINDER, "IOC exhausted available displayed liquidity"), events);
}
/// Armed orders activate only in their contract's regular session.
void check_triggers(State& s, Events& events) {
  for (std::size_t i = 0; i < s.orders.size(); ++i) {
    const auto& o = s.orders[i];
    if (o.status != OrderStatus::Armed ||
        md::trading_session(s.contracts.at(o.request.symbol).root, s.time).name != "regular" || !reached(s, o))
      continue;
    activate(s, o.id, events);
  }
}
/// Submit a reducer-owned market IOC that closes one position against the
/// current fresh book. Without executable liquidity nothing is recorded, so a
/// rule keeps retrying on later transactions instead of accumulating orders.
void flatten(State& s, const std::string& symbol, std::string_view why, Events& events) {
  const auto q = held(s, symbol);
  const auto contract = s.contracts.find(symbol);
  if (q == 0 || contract == s.contracts.end() || s.time >= contract->second.expiry_time() ||
      md::trading_session(contract->second.root, s.time).name != "regular" || !quote_check(s, symbol).ok())
    return;
  const auto side = q > 0 ? Side::Sell : Side::Buy;
  const auto& book = s.books.at(symbol);
  if ((side == Side::Sell ? book.bid_left : book.ask_left) <= 0) return;
  Order order;
  order.id = static_cast<OrderId>(s.orders.size() + 1);
  order.request = {"system:" + std::string(why) + ":" + std::to_string(order.id), symbol, side,
                   OrderType::Market, TimeInForce::Ioc, magnitude(q), {}, {}, {}};
  order.accepted_at = s.time;
  order.day_end = s.time;  // IOC: never rests past this transaction.
  order.system = true;
  s.orders.push_back(order);
  event(events, "order_accepted", order);
  match_symbols(s, {symbol}, events, order.id);
  auto& stored = s.orders.at(static_cast<std::size_t>(order.id - 1));
  if (stored.open()) cancel_order(stored, failure(Reason::IOC_REMAINDER, "IOC exhausted available displayed liquidity"), events);
}
void decide(State& s, EvaluationStatus status, Money equity, std::string message, Events& events) {
  auto& e = s.evaluation;
  e.status = status;
  e.decided_at = s.time;
  e.decided_equity = equity;
  e.decision = message;
  event(events, status == EvaluationStatus::Passed ? "evaluation_passed" : "evaluation_failed",
        Json{{"attempt", e.attempt}, {"equity", equity}, {"peak", e.peak}, {"floor", e.floor}, {"message", message}});
  for (auto& o : s.orders)
    if (!o.system) cancel_order(o, failure(Reason::EVALUATION_CLOSED, message), events);
}
/// Runs after every command: tracks the day's closing equity, ratchets an
/// intraday peak, decides pass/fail on fully marked equity (touching the floor
/// fails), then liquidates a decided attempt and auto-closes expiring positions.
void monitor_rules(State& s, Events& events) {
  const auto& rules = s.config.rules;
  auto& e = s.evaluation;
  // A journal created before any market data starts at time zero; the attempt
  // begins at the first real market time instead.
  if (e.started == 0 && s.time > 0) e.started = s.time;
  // Likewise the first payout cycle; journals from before payouts start it here too.
  if (e.cycle_started == 0) e.cycle_started = e.started;
  if (const auto equity = marked_equity(snapshot_of(s))) {
    if (local_date(s.time) == e.day) e.day_close_equity = *equity;
    if (rules.evaluation() && e.status == EvaluationStatus::Active) {
      if (rules.drawdown_mode == DrawdownMode::Intraday && *equity > e.peak) {
        e.peak = *equity;
        e.floor = floor_for(rules, e.peak, e.floor_locked);
      }
      const auto target = e.starting_balance + rules.profit_target;
      if (rules.max_drawdown > Money{} && *equity <= e.floor) {
        decide(s, EvaluationStatus::Failed, *equity, "Equity " + dollars(*equity) + " reached the drawdown floor " +
               dollars(e.floor) + " (peak " + dollars(e.peak) + ", max drawdown " + dollars(rules.max_drawdown) + ")", events);
      } else if (rules.profit_target > Money{} && *equity >= target) {
        decide(s, EvaluationStatus::Passed, *equity, "Equity " + dollars(*equity) + " reached the profit target " +
               dollars(target), events);
      }
    }
  }
  std::vector<std::string> symbols;
  for (const auto& [symbol, position] : s.ledger.positions()) symbols.push_back(symbol);
  for (const auto& symbol : symbols) {
    if (rules.evaluation() && e.status != EvaluationStatus::Active) {
      flatten(s, symbol, e.status == EvaluationStatus::Passed ? "target" : "drawdown", events);
      continue;
    }
    const auto& contract = s.contracts.at(symbol);
    if (rules.expiry_cutoff > 0 && s.time >= contract.expiry_time() - rules.expiry_cutoff &&
        s.time < contract.expiry_time()) {
      for (auto& o : s.orders)
        if (!o.system && o.request.symbol == symbol)
          cancel_order(o, failure(Reason::EXPIRY_CUTOFF, "Pre-expiry cutoff: the position is being closed"), events);
      flatten(s, symbol, "expiry", events);
    }
  }
}
void require_reason(const std::string& reason) {
  if (reason.find_first_not_of(" \t\r\n") == std::string::npos)
    throw TradingError(Reason::INVALID_REASON, "An explicit nonblank reason is required");
}
}  // namespace

PayoutQuote payout_quote(const TradingSnapshot& s, const AccountRules& rules) {
  const auto& p = rules.payouts;
  const auto& e = s.evaluation;
  PayoutQuote q;
  q.number = e.payouts.size() + 1;
  q.funded = rules.phase == Phase::Funded && p.qualifying_days > 0;
  q.active = e.status == EvaluationStatus::Active;
  q.flat = s.positions.empty() && s.open_orders.empty();
  q.qualifying_days = e.qualifying_days;
  q.required_days = p.qualifying_days;
  q.profit = s.equity - e.starting_balance;
  q.withdrawable = whole_cents(q.profit > Money{} ? q.profit.prorate(p.withdrawal_percent, 100) : Money{});
  if (!p.caps.empty()) q.cap = p.caps.at(std::min<std::size_t>(q.number, p.caps.size()) - 1);
  q.maximum = q.cap ? std::min(q.withdrawable, *q.cap) : q.withdrawable;
  // Touching the floor fails the account; an unlocked floor moves down with the
  // withdrawal, a locked one does not.
  if (rules.max_drawdown > Money{} && e.floor_locked)
    q.maximum = std::min(q.maximum, whole_cents(s.equity - e.floor - Money::from_micros(10'000)));
  q.minimum = p.minimum;
  q.trader_share = q.maximum.prorate(p.split_percent, 100);
  auto block = [&](Reason code, std::string message, std::optional<double> actual = {}, std::optional<double> limit = {}) {
    if (q.blocked.ok()) q.blocked = {code, std::move(message), actual, limit, "aggregate"};
  };
  if (!q.funded) block(Reason::PAYOUT_UNAVAILABLE, "Payouts are available on funded accounts");
  if (!q.active) block(Reason::PAYOUT_UNAVAILABLE, "The funded account is closed");
  if (!q.flat) block(Reason::PAYOUT_NOT_ELIGIBLE, "Close every position and working order before requesting a payout");
  if (static_cast<std::int64_t>(q.qualifying_days) < q.required_days)
    block(Reason::PAYOUT_NOT_ELIGIBLE, "Not enough qualifying days in this payout cycle",
          static_cast<double>(q.qualifying_days), static_cast<double>(q.required_days));
  if (q.maximum <= Money{} || q.maximum < q.minimum)
    block(Reason::PAYOUT_NOT_ELIGIBLE, "The payout available is below the minimum", q.maximum.dollars(), q.minimum.dollars());
  return q;
}

struct TradingSession::Impl {
  State state;
  std::shared_ptr<Journal> journal;
  std::shared_ptr<const TradingSnapshot> snapshot;
  bool stopped = false;

  CommandResult transact(Timestamp time, std::string_view type,
                         const std::function<CommandResult(State&, Events&)>& action) {
    if (stopped) throw TradingError(Reason::JOURNAL_IO, "Trading stopped after journal failure; recover first");
    State next = state;
    Events events;
    advance(next, time, events);
    auto result = action(next, events);
    monitor_loss(next, events);
    monitor_rules(next, events);
    if (next.version == std::numeric_limits<std::uint64_t>::max())
      throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Account version exhausted");
    ++next.version;
    auto publication = std::make_shared<TradingSnapshot>(snapshot_of(next));
    if (journal) {
      // Schema 2 adds account rules, evaluation progress, closures and system orders.
      // Tick policy v2 extends index-v1 with equity and ETF classes.
      const Json payload{{"schema", 2}, {"tick_policy", "v2"}, {"events", events},
                         {"state", next}, {"snapshot", *publication}, {"decision", result.decision}};
      try { journal->append(time, type, payload.dump()); }
      catch (...) {
        stopped = true;
        auto failed = std::make_shared<TradingSnapshot>(*snapshot);
        failed->journal_failed = true;
        failed->quality_flags.push_back(Reason::JOURNAL_IO);
        snapshot = std::move(failed);
        throw TradingError(Reason::JOURNAL_IO, "Journal commit failed; no in-memory transition published; stop trading and recover");
      }
    }
    state = std::move(next);
    snapshot = std::move(publication);
    result.account_version = state.version;
    return result;
  }
};
TradingSession::TradingSession(SessionConfig config, Timestamp time, std::shared_ptr<Journal> journal)
    : impl_(std::make_unique<Impl>()) {
  validate_limits(config.limits);
  validate_scenarios(config.scenarios);
  validate_rules(config.rules);
  if (time < 0) throw TradingError(Reason::INVALID_TIME, "Negative session time");
  if (config.fee_per_contract < Money{}) throw TradingError(Reason::INVALID_MONEY, "Fee cannot be negative");
  if (journal && journal->sequence() != 0) throw TradingError(Reason::JOURNAL_CORRUPT, "Use recover for a nonempty journal");
  impl_->state.config = std::move(config);
  impl_->state.time = time;
  impl_->state.day = local_date(time);
  impl_->state.ledger = Ledger(impl_->state.config.initial_cash);
  impl_->state.start_equity = impl_->state.config.initial_cash;
  impl_->state.evaluation = fresh_evaluation(impl_->state, 1);
  impl_->journal = std::move(journal);
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
  impl_->transact(time, "session_start", [](State& s, Events& events) {
    event(events, "session_start", s.config);
    return CommandResult{};
  });
}
TradingSession::TradingSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TradingSession::~TradingSession() = default;
TradingSession::TradingSession(TradingSession&&) noexcept = default;
TradingSession& TradingSession::operator=(TradingSession&&) noexcept = default;
CommandResult TradingSession::define(const md::OptionContract& contract, Timestamp time) {
  return impl_->transact(time, "definition", [&](State& s, Events& events) {
    const auto decision = eligible(contract);
    if (!decision.ok()) return CommandResult{decision, {}, 0};
    const auto symbol = contract.osi_symbol();
    const auto old = s.contracts.find(symbol);
    if (old != s.contracts.end() && Json(old->second) != Json(contract))
      return CommandResult{failure(Reason::INVALID_CONTRACT, "OSI definition conflicts with registered terms"), {}, 0};
    s.contracts[symbol] = contract;
    event(events, "definition", contract);
    return CommandResult{};
  });
}
CommandResult TradingSession::submit(OrderRequest request, Timestamp time, Decision rejection) {
  return impl_->transact(time, "submit", [&](State& s, Events& events) {
    monitor_loss(s, events);
    Order order;
    order.id = static_cast<OrderId>(s.orders.size() + 1);
    order.request = std::move(request);
    order.accepted_at = time;
    const bool duplicate = std::any_of(s.orders.begin(), s.orders.end(), [&](const auto& o) {
      return o.request.client_order_id == order.request.client_order_id;
    });
    s.orders.push_back(order);
    auto& stored = s.orders.back();
    auto decision = duplicate ? failure(Reason::DUPLICATE_CLIENT_ID, "client_order_id already used") : !rejection.ok() ? rejection : order_check(s, stored);
    if (!decision.ok()) {
      stored.status = OrderStatus::Rejected;
      stored.reason = decision;
      event(events, "order_rejected", stored);
      return CommandResult{decision, stored.id, 0};
    }
    const auto id = stored.id;
    const auto& contract = s.contracts.at(stored.request.symbol);
    if (stored.request.trigger) {
      // Armed until reached, and good until expiry; a level already reached
      // activates at once.
      stored.status = OrderStatus::Armed;
      stored.day_end = contract.expiry_time();
      event(events, "order_accepted", stored);
      if (md::trading_session(contract.root, s.time).name == "regular" &&
          reached(s, s.orders.at(static_cast<std::size_t>(id - 1))))
        activate(s, id, events);
      return CommandResult{{}, id, 0};
    }
    stored.day_end = regular_end(contract, time);
    event(events, "order_accepted", stored);
    // Existing better orders share any remaining budget even on command ingress.
    // Matching can append bracket exits, so re-read the order by ID afterwards.
    match_symbols(s, {s.orders.at(static_cast<std::size_t>(id - 1)).request.symbol}, events, id);
    auto& accepted = s.orders.at(static_cast<std::size_t>(id - 1));
    if (accepted.open() && accepted.request.tif == TimeInForce::Ioc)
      cancel_order(accepted, failure(Reason::IOC_REMAINDER, "IOC exhausted available displayed liquidity"), events);
    return CommandResult{{}, id, 0};
  });
}
CommandResult TradingSession::cancel(OrderId id, Timestamp time) {
  return impl_->transact(time, "cancel", [&](State& s, Events& events) {
    if (id == 0 || id > s.orders.size()) return CommandResult{failure(Reason::UNKNOWN_ORDER, "Unknown order ID"), {}, 0};
    auto& order = s.orders.at(static_cast<std::size_t>(id - 1));
    if (!order.open()) return CommandResult{failure(Reason::ORDER_TERMINAL, "Order is already terminal"), id, 0};
    cancel_order(order, failure(Reason::USER_CANCEL, "Cancelled by caller"), events);
    return CommandResult{{}, id, 0};
  });
}
CommandResult TradingSession::on_quotes(const std::vector<QuoteObservation>& quotes,
    const std::vector<Valuation>& valuations, Timestamp time) {
  return impl_->transact(time, "market", [&](State& s, Events& events) {
    std::set<std::string> seen;
    std::set<std::string> changed;
    for (const auto& quote : quotes) {
      if (!s.contracts.contains(quote.symbol)) throw TradingError(Reason::UNKNOWN_CONTRACT, "Quote references unregistered OSI");
      if (quote.time < 0 || quote.time > time) throw TradingError(Reason::INVALID_TIME, "Quote is future-dated or negative");
      if (!seen.insert(quote.symbol).second) throw TradingError(Reason::INVALID_QUOTE, "One observation per contract per batch is required");
      auto& book = s.books[quote.symbol];
      if (quote.observation <= book.quote.observation || quote.time < book.quote.time) continue;
      book = {quote, valid_quote(quote) ? quote.bid_size : 0, valid_quote(quote) ? quote.ask_size : 0};
      changed.insert(quote.symbol);
      if (valid_quote(quote) && time - quote.time <= s.config.limits.max_quote_age)
        s.marks[quote.symbol] = {mid(quote), quote.time};
    }
    seen.clear();
    for (auto valuation : valuations) {
      if (!s.contracts.contains(valuation.symbol)) throw TradingError(Reason::UNKNOWN_CONTRACT, "Valuation references unregistered OSI");
      if (valuation.time < 0 || valuation.time > time) throw TradingError(Reason::INVALID_TIME, "Valuation is future-dated or negative");
      if (!seen.insert(valuation.symbol).second) throw TradingError(Reason::MISSING_VALUATION, "One valuation per contract per batch is required");
      const auto prior = s.valuations.find(valuation.symbol);
      if (prior != s.valuations.end() && valuation.time < prior->second.time) continue;
      if (!valid_valuation(valuation)) {
        // Null/nonfinite data is explicitly invalid, never implicitly zero risk.
        valuation = Valuation{valuation.symbol, valuation.time, 0, 0, 0, 0, 0, 0, 0, 0, 0, false};
      }
      s.valuations[valuation.symbol] = valuation;
    }
    monitor_loss(s, events);
    // Invalid quotes provide no liquidity. Keep orders until a new valid quote
    // permits the fill-time risk check (or a clock/kill command cancels them).
    for (auto it = changed.begin(); it != changed.end();) {
      if (!quote_check(s, *it).ok()) it = changed.erase(it); else ++it;
    }
    if (!s.kill) {
      match_symbols(s, changed, events);
      // Triggers read the batch's books and valuations after resting orders match.
      check_triggers(s, events);
    }
    return CommandResult{};
  });
}
CommandResult TradingSession::set_limits(Limits limits, Timestamp time) {
  validate_limits(limits);
  return impl_->transact(time, "limit_change", [&](State& s, Events& events) {
    s.config.limits = std::move(limits);
    if (s.limits_revision == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Limits revision exhausted");
    ++s.limits_revision;
    event(events, "limit_change", s.config.limits);
    monitor_loss(s, events);
    for (auto& order : s.orders) {
      if (!order.open()) continue;
      auto d = order_check(s, order);
      if (!d.ok()) {
        d.message = std::string(to_string(d.code)) + ": " + d.message;
        d.code = Reason::RISK_CHANGED;
        cancel_order(order, d, events);
      }
    }
    return CommandResult{};
  });
}
CommandResult TradingSession::trip_kill(std::string reason, Timestamp time) {
  require_reason(reason);
  return impl_->transact(time, "kill_trip", [&](State& s, Events& events) {
    trip(s, reason, events);
    return CommandResult{};
  });
}
CommandResult TradingSession::reset_kill(std::string reason, Timestamp time) {
  require_reason(reason);
  return impl_->transact(time, "kill_reset", [&](State& s, Events& events) {
    s.kill = false;
    s.kill_reason.clear();
    event(events, "kill_reset", Json{{"reason", reason}});
    monitor_loss(s, events);
    if (s.kill) return CommandResult{loss_check(s, snapshot_of(s)), {}, 0};
    return CommandResult{};
  });
}
CommandResult TradingSession::settle(const std::string& symbol, Money settlement, Timestamp time) {
  return impl_->transact(time, "settlement", [&](State& s, Events& events) {
    const auto it = s.contracts.find(symbol);
    if (it == s.contracts.end()) return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "Unknown settlement contract"), {}, 0};
    if (s.settled.contains(symbol)) return CommandResult{failure(Reason::ALREADY_SETTLED, "Settlement is exactly once per OSI"), {}, 0};
    if (time < it->second.expiry_time() || settlement < Money{} || !s.ledger.positions().contains(symbol))
      return CommandResult{failure(Reason::INVALID_SETTLEMENT, "Settlement requires expiry, a position and nonnegative reference"), {}, 0};
    const Money strike = Money::from_double(it->second.strike);
    const Money intrinsic = std::max(Money{}, it->second.type == pricing::OptionType::Call ? settlement - strike : strike - settlement);
    const auto quantity = held(s, symbol);
    s.ledger.settle(symbol, intrinsic);
    s.settled.insert(symbol);
    s.closures.push_back({symbol, quantity, intrinsic, time, ClosureKind::Settlement, s.fills.size()});
    sync_exits(s, symbol, events);
    event(events, "settlement", Json{{"symbol", symbol}, {"reference", settlement}, {"intrinsic", intrinsic}});
    return CommandResult{};
  });
}
CommandResult TradingSession::roll_day(Timestamp time) {
  return impl_->transact(time, "day_rollover", [&](State& s, Events& events) {
    const auto day = local_date(time);
    if (day <= s.day) return CommandResult{failure(Reason::INVALID_TIME, "Rollover requires a later New York date"), {}, 0};
    monitor_loss(s, events);
    const auto snapshot = snapshot_of(s);
    if (!snapshot.valuation_complete) return CommandResult{failure(Reason::STALE_QUOTE, "Rollover requires complete marked equity"), {}, 0};
    // Close the finished day. An end-of-day floor ratchets only here, from the
    // last fully marked equity observed on that date; breaches are still checked
    // on every transaction by monitor_rules.
    auto& e = s.evaluation;
    const auto& rules = s.config.rules;
    if (rules.evaluation() && e.status == EvaluationStatus::Active &&
        rules.drawdown_mode == DrawdownMode::EndOfDay && e.day_close_equity > e.peak) {
      e.peak = e.day_close_equity;
      e.floor = floor_for(rules, e.peak, e.floor_locked);
    }
    // A placeholder date from before the attempt started is not a trading day.
    // On a funded account, a day with enough net realised profit counts once,
    // toward the payout cycle in progress when it closes.
    if (e.started > 0 && e.day >= local_date(e.started)) {
      const auto realised = net_realised(s) - e.day_open_realised;
      const auto& payouts = rules.payouts;
      const bool qualifying = rules.phase == Phase::Funded && e.status == EvaluationStatus::Active &&
          payouts.qualifying_days > 0 && realised >= payouts.qualifying_profit && realised > Money{};
      if (qualifying) ++e.qualifying_days;
      e.days.push_back({e.day, e.day_open_equity, e.day_close_equity, e.peak, e.floor, realised, qualifying});
      event(events, "evaluation_day", e.days.back());
    }
    e.day_open_realised = net_realised(s);
    e.day = day;
    e.day_open_equity = snapshot.equity;
    e.day_close_equity = snapshot.equity;
    s.start_equity = snapshot.equity;
    s.day = day;
    event(events, "day_rollover", Json{{"day", day}, {"equity", s.start_equity}});
    return CommandResult{};
  });
}
CommandResult TradingSession::request_payout(Money amount, Timestamp time) {
  if (amount <= Money{} || amount.micros() % 10'000 != 0)
    throw TradingError(Reason::INVALID_PAYOUT, "Payout amount must be a positive whole number of cents");
  return impl_->transact(time, "payout", [&](State& s, Events& events) {
    const auto& rules = s.config.rules;
    auto& e = s.evaluation;
    const auto snapshot = snapshot_of(s);
    const auto quote = payout_quote(snapshot, rules);
    auto reject = [&](std::string message, Money limit) {
      return CommandResult{{Reason::INVALID_PAYOUT, std::move(message), amount.dollars(), limit.dollars(), "aggregate"}, {}, 0};
    };
    if (!quote.blocked.ok()) return CommandResult{quote.blocked, {}, 0};
    if (amount < quote.minimum) return reject("Below the minimum payout", quote.minimum);
    if (amount > quote.maximum) return reject("Above the maximum for this payout", quote.maximum);
    const Money equity = snapshot.equity;  // Flat, so this is cash.
    s.ledger.withdraw(amount);
    // The trading day in progress takes the withdrawal, even after its date
    // ends and before rollover, so its P&L and an end-of-day ratchet ignore it.
    s.start_equity = s.start_equity - amount;
    e.day_open_equity = e.day_open_equity - amount;
    e.day_close_equity = e.day_close_equity - amount;
    if (!e.floor_locked) {
      e.peak = e.peak - amount;
      e.floor = floor_for(rules, e.peak, e.floor_locked);
    }
    e.payouts.push_back({quote.number, s.time, e.day, amount, amount.prorate(rules.payouts.split_percent, 100), equity});
    e.qualifying_days = 0;
    e.cycle_started = s.time;
    event(events, "payout", e.payouts.back());
    return CommandResult{};
  });
}
CommandResult TradingSession::reset_account(Money initial_cash, AccountRules rules, std::string reason, Timestamp time) {
  require_reason(reason);
  validate_rules(rules);
  if (initial_cash <= Money{}) throw TradingError(Reason::INVALID_MONEY, "Starting balance must be positive");
  return impl_->transact(time, "account_reset", [&](State& s, Events& events) {
    const auto snapshot = snapshot_of(s);
    for (auto& o : s.orders) cancel_order(o, failure(Reason::ACCOUNT_RESET, reason), events);
    for (const auto& p : snapshot.positions) {
      const auto& position = p.position;
      s.closures.push_back({position.contract.osi_symbol(), position.quantity,
                            p.mark ? *p.mark : average_unit_price(position), s.time, ClosureKind::Reset, s.fills.size()});
    }
    const auto& e = s.evaluation;
    s.attempts.push_back({e.attempt, s.config.rules.plan, e.started, s.time, e.starting_balance, snapshot.equity,
                          e.status, e.decision, e.first_order, e.first_fill});
    const auto attempt = e.attempt + 1;
    s.config.initial_cash = initial_cash;
    s.config.rules = std::move(rules);
    s.ledger = Ledger(initial_cash);
    s.start_equity = initial_cash;
    s.kill = false;
    s.kill_reason.clear();
    s.evaluation = fresh_evaluation(s, attempt);
    event(events, "account_reset", Json{{"reason", reason}, {"attempt", attempt}, {"initial_cash", initial_cash},
                                        {"rules", s.config.rules}});
    return CommandResult{};
  });
}
std::shared_ptr<const TradingSnapshot> TradingSession::snapshot() const { return impl_->snapshot; }
std::string TradingSession::snapshot_json() const { return Json(*impl_->snapshot).dump(); }
const SessionConfig& TradingSession::config() const { return impl_->state.config; }
const std::map<std::string, md::OptionContract>& TradingSession::contracts() const { return impl_->state.contracts; }
const std::map<std::string, Valuation>& TradingSession::valuations() const { return impl_->state.valuations; }
std::optional<QuoteObservation> TradingSession::quote(const std::string& symbol) const {
  const auto it = impl_->state.books.find(symbol);
  return it == impl_->state.books.end() ? std::nullopt : std::optional(it->second.quote);
}
md::Date TradingSession::trading_day() const { return impl_->state.day; }
TradingSession TradingSession::recover(const JournalRecovery& recovery, std::shared_ptr<Journal> journal) {
  if (recovery.records.empty()) throw TradingError(Reason::JOURNAL_CORRUPT, "Recovery requires a session_start record");
  // Reverify even caller-constructed recovery objects instead of trusting them.
  std::string lines;
  try {
    for (const auto& r : recovery.records) {
      lines += Json{{"seq", r.seq}, {"time", r.time}, {"type", r.type}, {"payload", Json::parse(r.payload)},
                    {"prev_hash", r.prev_hash}, {"hash", r.hash}}.dump() + '\n';
    }
  } catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
  const auto verified = verify_journal(lines, recovery.head);
  if (verified.records.front().type != "session_start") throw TradingError(Reason::JOURNAL_CORRUPT, "Missing session start");
  if (journal && (recovery.truncated_final_line || journal->head() != verified.head || journal->sequence() != verified.records.size()))
    throw TradingError(Reason::JOURNAL_CORRUPT, "Recovery sink does not match verified journal head");
  auto impl = std::make_unique<Impl>();
  bool legacy = false;
  try {
    for (const auto& r : verified.records) {
      const auto payload = Json::parse(r.payload);
      const auto& schema = payload.at("schema");
      const auto& ticks = payload.at("tick_policy");
      if ((schema != 1 && schema != 2) || (ticks != "index-v1" && ticks != "v2"))
        throw TradingError(Reason::JOURNAL_CORRUPT, "Unsupported trading journal schema/policy");
      legacy = schema == 1;
      auto state = payload.at("state").get<State>();
      auto snapshot = payload.at("snapshot").get<TradingSnapshot>();
      if (state.version != r.seq || state.time != r.time || snapshot.account_version != state.version || snapshot.time != state.time)
        throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded state version/time does not match transaction");
      validate_limits(state.config.limits);
      validate_scenarios(state.config.scenarios);
      validate_rules(state.config.rules);
      impl->state = std::move(state);
      impl->snapshot = std::make_shared<TradingSnapshot>(std::move(snapshot));
    }
    if (legacy) {
      // A schema 1 account has no rules; start its progress record from the
      // journal's first transaction so later schema 2 records continue it.
      auto& s = impl->state;
      Evaluation e;
      for (const auto& r : verified.records) if (r.time > 0) { e.started = r.time; break; }
      e.starting_balance = s.config.initial_cash;
      e.peak = e.starting_balance;
      e.day = s.day;
      e.day_open_equity = s.start_equity;
      e.day_close_equity = impl->snapshot->equity;
      e.day_open_realised = net_realised(s);  // Unknown for the day in progress; count from here.
      e.cycle_started = e.started;
      s.evaluation = std::move(e);
      auto snapshot = std::make_shared<TradingSnapshot>(*impl->snapshot);
      snapshot->evaluation = s.evaluation;
      snapshot->buying_power = buying_power(s).total;
      impl->snapshot = std::move(snapshot);
    }
  } catch (const TradingError& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
    catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
  impl->journal = std::move(journal);
  return TradingSession(std::move(impl));
}
}  // namespace openport::trading

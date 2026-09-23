#include "openport/trading/session.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "state.hpp"
#include "state_delta.hpp"

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
bool opens(Quantity held_quantity, Quantity signed_fill) {
  return held_quantity == 0 || (held_quantity > 0) == (signed_fill > 0) || magnitude(signed_fill) > magnitude(held_quantity);
}
/// Whether an order trades `symbol`, as its contract or one of its legs.
bool touches(const OrderRequest& r, const std::string& symbol) {
  return r.symbol == symbol || std::any_of(r.legs.begin(), r.legs.end(), [&](const Leg& leg) { return leg.symbol == symbol; });
}
Quantity signed_contracts(const Leg& leg, Quantity units) { return leg.side == Side::Buy ? units * leg.ratio : -units * leg.ratio; }
/// A multi-leg order's net debit per unit at the far sides: asks for bought
/// legs, bids for sold legs. Nothing without a valid book on every leg.
std::optional<Money> executable_net(const State& s, const OrderRequest& r) {
  Money net;
  for (const auto& leg : r.legs) {
    const auto book = s.books.find(leg.symbol);
    if (book == s.books.end() || !valid_quote(book->second.quote)) return std::nullopt;
    const auto& q = book->second.quote;
    const auto price = (leg.side == Side::Buy ? *q.ask : *q.bid) * leg.ratio;
    net = leg.side == Side::Buy ? net + price : net - price;
  }
  return net;
}
/// Positions for margin by symbol: signed contracts and, for shorts, their
/// buy-back value.
using MarginBook = std::map<std::string, std::pair<Quantity, Money>>;
/// The held positions; a short's buy-back value is its mark, or its entry credit without one.
MarginBook held_book(const State& s) {
  MarginBook book;
  for (const auto& [symbol, p] : s.ledger.positions()) {
    if (p.quantity == 0) continue;
    Money value;
    if (p.quantity < 0) {
      const auto mark = s.marks.find(symbol);
      value = mark != s.marks.end() ? (mark->second.price * 100) * -p.quantity : -p.basis;
    }
    book[symbol] = {p.quantity, value};
  }
  return book;
}
/// Trade `change` contracts of `symbol` in the book: shorts that remain keep
/// their share of the buy-back value, and contracts newly sold are valued at `price`.
void trade(MarginBook& book, const std::string& symbol, Quantity change, Money price) {
  const auto it = book.find(symbol);
  const auto [quantity, value] = it == book.end() ? std::pair<Quantity, Money>{0, {}} : it->second;
  const auto next = quantity + change;
  if (next == 0) { book.erase(symbol); return; }
  Money next_value;
  if (next < 0) {
    const Quantity kept = quantity < 0 ? std::min(-quantity, -next) : 0;
    next_value = (kept > 0 ? value.prorate(kept, -quantity) : Money{}) + (price * 100) * (-next - kept);
  }
  book[symbol] = {next, next_value};
}
Money requirement(const State& s, const MarginBook& book) {
  std::vector<MarginLeg> legs;
  for (const auto& [symbol, entry] : book) legs.push_back({s.contracts.at(symbol), entry.first, entry.second, spot_for(s, symbol)});
  return margin_requirement(legs);
}
/// Buying power that positions do not hold: cash less their margin requirement.
Money free_power(const State& s) { return s.ledger.account().cash - requirement(s, held_book(s)); }
struct Use {
  Money reservation;   ///< Fees plus any net cost, never less than the fees.
  bool uses = false;   ///< Filling would reduce free buying power.
};
/// An order's use of buying power if it filled now: its fees, plus the change
/// in the margin requirement from `before` to `after`, plus the premium it pays
/// (`cash`, negative when it receives premium). An order that releases more
/// than it costs reserves only its fees.
Use use_of(const State& s, const MarginBook& before, const MarginBook& after, Money cash, Money fees) {
  const auto change = requirement(s, after) - requirement(s, before) + cash;
  return {fees + std::max(Money{}, change), fees + change > Money{}};
}
/// A multi-leg order on the held positions: legs sold short at their marks,
/// and the net debit (or credit) per unit at its limit or the current far sides.
Use combo_use(const State& s, const Order& o, const MarginBook& book) {
  const auto units = o.remaining();
  auto after = book;
  Money fees;
  for (const auto& leg : o.request.legs) {
    const auto contracts = signed_contracts(leg, units);
    fees = fees + s.config.fee_per_contract * magnitude(contracts);
    const auto mark = s.marks.find(leg.symbol);
    trade(after, leg.symbol, contracts, mark != s.marks.end() ? mark->second.price : Money{});
  }
  const auto net = o.request.limit_price ? *o.request.limit_price : executable_net(s, o.request).value_or(Money{});
  return use_of(s, book, after, (net * 100) * units, fees);
}
Money average_unit_price(const Position& p) {
  const auto size = magnitude(p.quantity);
  if (size == 0) return {};
  return (p.basis < Money{} ? -p.basis : p.basis).prorate(1, size * 100);
}
struct PowerDetail {
  BuyingPower total;
  Money focus_reservation;
  Quantity focus_opening = 0;  ///< Contracts the focus order opens beyond earlier orders' claims.
  bool focus_uses = false;     ///< The focus order would reduce free buying power.
};
/// Positions hold their margin requirement (margin_requirement, spreads netted).
/// Each working order reserves what filling it now would cost (use_of): fees,
/// the change in margin on the held positions and the premium it pays, less
/// what it receives, never below the fees. Single-leg orders see the positions
/// less the contracts earlier orders already claim to close, in acceptance
/// order, so two sells cannot both claim the same long. Bracket exits stay
/// within their position and reserve only fees. Orders without a limit use the
/// current far side.
PowerDetail buying_power(const State& s, OrderId focus = 0) {
  PowerDetail out;
  const auto book = held_book(s);
  const Money short_requirement = requirement(s, book);
  std::map<std::string, std::pair<Quantity, Quantity>> capacity;
  Money reserved;
  for (const auto& o : s.orders) {
    if (!o.open() || o.remaining() <= 0 || shadowed(s, o)) continue;
    const auto remaining = o.remaining();
    Use use;
    Quantity opening = 0;
    if (multi_leg(o.request)) {
      use = combo_use(s, o, book);
      for (const auto& leg : o.request.legs)
        if (opens(held(s, leg.symbol), signed_contracts(leg, remaining))) opening = remaining;
    } else {
      const auto& symbol = o.request.symbol;
      if (!s.contracts.contains(symbol)) continue;
      const auto q = held(s, symbol);
      auto& [long_left, short_left] = capacity.try_emplace(symbol, std::max<Quantity>(q, 0), std::max<Quantity>(-q, 0)).first->second;
      const bool buy = o.request.side == Side::Buy;
      Money price;
      if (o.request.limit_price) price = *o.request.limit_price;
      else if (const auto quote = s.books.find(symbol); quote != s.books.end() && valid_quote(quote->second.quote))
        price = buy ? *quote->second.quote.ask : *quote->second.quote.bid;
      const Money fees = s.config.fee_per_contract * remaining;
      if (o.role != OrderRole::Normal) {
        // Bracket exits stay within the position, so they only ever close; they
        // leave closing capacity to ordinary orders such as a manual close.
        use = {fees, false};
      } else {
        auto before = book;
        if (q > 0 && !buy) trade(before, symbol, long_left - q, {});
        if (q < 0 && buy) trade(before, symbol, -q - short_left, {});
        auto& left = buy ? short_left : long_left;
        const auto closing = std::min(remaining, left);
        left -= closing;
        opening = remaining - closing;
        auto after = before;
        trade(after, symbol, buy ? remaining : -remaining, price);
        const Money premium = (price * 100) * remaining;
        use = use_of(s, before, after, buy ? premium : -premium, fees);
      }
    }
    reserved = reserved + use.reservation;
    if (o.id == focus) { out.focus_reservation = use.reservation; out.focus_opening = opening; out.focus_uses = use.uses; }
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
    auto expiry = std::numeric_limits<Timestamp>::max();
    for (const auto& symbol : order_symbols(o.request)) expiry = std::min(expiry, s.contracts.at(symbol).expiry_time());
    if (time >= expiry) cancel_order(o, failure(Reason::EXPIRED, "Contract reached expiry and awaits settlement"), events);
    else if (time >= o.day_end) cancel_order(o, failure(Reason::DAY_END, "Regular session ended"), events);
  }
}
Decision account_check(const State& s) {
  if (s.kill) return failure(Reason::KILL_SWITCH, s.kill_reason);
  if (s.config.rules.evaluation() && s.evaluation.status != EvaluationStatus::Active)
    return failure(Reason::EVALUATION_CLOSED, std::string("The evaluation has ") +
        (s.evaluation.status == EvaluationStatus::Passed ? "passed" : "failed") +
        "; reset the account to start a new attempt");
  return {};
}
/// Checks a multi-leg order like a single-leg one, per leg where it applies:
/// contracts, sessions, size and quotes on every leg; the net price on the
/// smallest leg tick and inside the band around the net mid (as wide as the
/// band for the legs' gross premium); then rules, exposure and buying power.
Decision combo_check(const State& s, const Order& o, bool at_fill) {
  const auto& r = o.request;
  const auto& rules = s.config.rules;
  const bool shape = r.legs.size() >= 2 && r.legs.size() <= kMaxLegs && r.symbol.empty() && r.side == Side::Buy &&
      !r.trigger && !r.bracket && !r.client_order_id.empty() && r.quantity > 0 &&
      ((r.type == OrderType::Market && r.tif == TimeInForce::Ioc && !r.limit_price) ||
       (r.type == OrderType::Limit && r.limit_price && (r.tif == TimeInForce::Day || r.tif == TimeInForce::Ioc)));
  if (!shape)
    return failure(Reason::INVALID_ORDER, "Multi-leg orders take two to four legs, a unit quantity and a net limit "
                   "(negative for a credit) or market IOC, and no trigger or bracket");
  std::set<std::string> seen;
  const md::OptionContract* first = nullptr;
  Money tick;
  for (const auto& leg : r.legs) {
    if (leg.ratio < 1 || leg.ratio > kMaxRatio || (leg.side != Side::Buy && leg.side != Side::Sell) || !seen.insert(leg.symbol).second)
      return failure(Reason::INVALID_ORDER, "Each leg needs its own contract, a side and a ratio from 1 to 10");
    const auto c = s.contracts.find(leg.symbol);
    if (c == s.contracts.end()) return failure(Reason::UNKNOWN_CONTRACT, "Every leg must reference a registered canonical OSI definition");
    if (first && c->second.underlying != first->underlying) return failure(Reason::INVALID_ORDER, "All legs must share one underlying");
    first = &c->second;
    if (s.time >= c->second.expiry_time()) return failure(Reason::EXPIRED, "A leg's contract has expired");
    if (md::trading_session(c->second.root, s.time).name != "regular")
      return failure(Reason::SESSION_CLOSED, "v1 accepts and executes only in the product regular session");
    if (r.quantity > s.config.limits.max_order_contracts / leg.ratio)
      return {Reason::MAX_ORDER_CONTRACTS, "A leg's contract count exceeds the order limit",
              static_cast<double>(r.quantity) * static_cast<double>(leg.ratio), static_cast<double>(s.config.limits.max_order_contracts), leg.symbol};
    const auto leg_tick = tick_size(c->second.root, Money{});
    tick = tick == Money{} ? leg_tick : std::min(tick, leg_tick);
  }
  if (r.limit_price && r.limit_price->micros() % tick.micros() != 0)
    return failure(Reason::INVALID_TICK, "Net price is not a multiple of the legs' smallest tick");
  if (rules.buy_only) return failure(Reason::BUY_ONLY, "This plan is buy-only and single-leg; multi-leg orders need a plan that allows any strategy");
  for (const auto& leg : r.legs) {
    if (rules.expiry_cutoff > 0 && s.time >= s.contracts.at(leg.symbol).expiry_time() - rules.expiry_cutoff)
      return failure(Reason::EXPIRY_CUTOFF, "A leg is inside the pre-expiry cutoff; close positions with single-leg orders");
    if (auto d = quote_check(s, leg.symbol); !d.ok()) { d.scope = leg.symbol; return d; }
  }
  const auto net = *executable_net(s, r);
  Money middle, gross;
  for (const auto& leg : r.legs) {
    const auto value = mid(s.books.at(leg.symbol).quote) * leg.ratio;
    middle = leg.side == Side::Buy ? middle + value : middle - value;
    gross = gross + value;
  }
  const auto price = !at_fill && r.limit_price ? *r.limit_price : net;
  const long double difference = std::abs(static_cast<long double>(price.micros()) - static_cast<long double>(middle.micros()));
  const long double band = std::max(static_cast<long double>(s.config.limits.price_band_absolute.micros()),
      static_cast<long double>(s.config.limits.price_band_relative) * static_cast<long double>(gross.micros()));
  if (difference > band)
    return {Reason::PRICE_BAND, "Net price is outside the configured band around the net mid",
            static_cast<double>(difference / 1'000'000), static_cast<double>(band / 1'000'000), first->underlying};
  const auto snapshot = snapshot_of(s);
  if (!snapshot.valuation_complete) return failure(Reason::STALE_QUOTE, "All held positions need fresh marks before trading");
  if (const auto d = loss_check(s, snapshot); !d.ok()) return d;
  if (const auto d = check_exposure(snapshot.risk); !d.ok()) return d;
  if (rules.buying_power && !at_fill) {
    const auto power = buying_power(s, o.id);
    if (power.focus_uses && power.total.available < Money{})
      return {Reason::BUYING_POWER, power.focus_opening > 0 ? "Order needs more buying power than the account has available"
                : "Closing these legs uncovers a short option; close the short legs too, or first",
              power.focus_reservation.dollars(), (power.total.available + power.focus_reservation).dollars(), first->underlying};
  }
  return {};
}
Decision order_check(const State& s, const Order& o, bool at_fill = false) {
  if (const auto d = account_check(s); !d.ok()) return d;
  if (multi_leg(o.request)) return combo_check(s, o, at_fill);
  const auto& rules = s.config.rules;
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
    // Orders that free buying power are always allowed; fills recheck against
    // the projected ledger instead.
    const auto power = buying_power(s, o.id);
    if (power.focus_uses && power.total.available < Money{})
      return {Reason::BUYING_POWER, power.focus_opening > 0 ? "Order needs more buying power than the account has available"
                : "Selling this long uncovers a short option it protects; buy the short back first, or close both together as one order",
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
    State projected = s;
    projected.ledger.fill(s.contracts.at(o.request.symbol), signed_quantity, price, fee);
    projected.orders.at(static_cast<std::size_t>(id - 1)).filled_quantity += quantity;
    decision = loss_check(projected, snapshot_of(projected));
    // A fill that reduces free buying power must leave it nonnegative.
    if (decision.ok() && s.config.rules.buying_power && free_power(projected) < free_power(s)) {
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
/// A multi-leg order fills all its legs together, in ratio, at each leg's far
/// side when the net debit is at or below its limit; units are bounded by every
/// leg's remaining displayed size. The whole projected fill is rechecked first.
void match_combo(State& s, OrderId id, Events& events, std::optional<OrderId> incoming) {
  auto& o = s.orders.at(static_cast<std::size_t>(id - 1));
  if (!o.open()) return;
  for (const auto& leg : o.request.legs) {
    if (!quote_check(s, leg.symbol).ok()) return;
    if (incoming != id && s.books.at(leg.symbol).quote.time < o.accepted_at) return;
  }
  const auto net = executable_net(s, o.request);
  if (!net || (o.request.limit_price && *net > *o.request.limit_price)) return;
  auto decision = order_check(s, o, true);
  Quantity units = o.remaining();
  for (const auto& leg : o.request.legs) {
    const auto& book = s.books.at(leg.symbol);
    units = std::min(units, (leg.side == Side::Buy ? book.ask_left : book.bid_left) / leg.ratio);
  }
  if (decision.ok() && units <= 0) return;
  if (decision.ok()) {
    State projected = s;
    for (const auto& leg : o.request.legs) {
      const auto& q = s.books.at(leg.symbol).quote;
      const auto contracts = signed_contracts(leg, units);
      projected.ledger.fill(s.contracts.at(leg.symbol), contracts, leg.side == Side::Buy ? *q.ask : *q.bid,
                            s.config.fee_per_contract * magnitude(contracts));
    }
    projected.orders.at(static_cast<std::size_t>(id - 1)).filled_quantity += units;
    decision = loss_check(projected, snapshot_of(projected));
    if (decision.ok() && s.config.rules.buying_power && free_power(projected) < free_power(s)) {
      const auto power = buying_power(projected).total;
      if (power.available < Money{})
        decision = {Reason::BUYING_POWER, "Fill needs more buying power than the account has available",
                    (-power.available).dollars(), 0.0, s.contracts.at(o.request.legs.front().symbol).underlying};
    }
  }
  if (!decision.ok()) {
    decision.message = std::string(to_string(decision.code)) + ": " + decision.message;
    decision.code = Reason::RISK_CHANGED;
    cancel_order(o, decision, events);
    return;
  }
  for (const auto& leg : o.request.legs) {
    auto& book = s.books.at(leg.symbol);
    const auto contracts = signed_contracts(leg, units);
    const auto size = magnitude(contracts);
    const Money price = leg.side == Side::Buy ? *book.quote.ask : *book.quote.bid;
    const Money fee = s.config.fee_per_contract * size;
    s.ledger.fill(s.contracts.at(leg.symbol), contracts, price, fee);
    (leg.side == Side::Buy ? book.ask_left : book.bid_left) -= size;
    Fill fill{static_cast<std::uint64_t>(s.fills.size() + 1), id, leg.symbol, leg.side,
              size, price, fee, book.quote.observation, book.quote.time, s.time};
    s.fills.push_back(fill);
    event(events, "fill", fill);
  }
  o.filled_quantity += units;
  o.filled_notional = o.filled_notional + *net * units;
  o.status = o.remaining() == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
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
  // Multi-leg orders then take the displayed liquidity left, in acceptance order.
  std::vector<OrderId> combos;
  for (const auto& o : s.orders)
    if (o.open() && multi_leg(o.request) &&
        std::any_of(o.request.legs.begin(), o.request.legs.end(), [&](const Leg& leg) { return symbols.contains(leg.symbol); }))
      combos.push_back(o.id);
  for (const auto id : combos) match_combo(s, id, events, incoming);
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
                    entry.filled_quantity, spec.limit_price, spec.trigger, {}, {}};
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
  for (const auto& symbol : order_symbols(s.orders.at(static_cast<std::size_t>(id - 1)).request)) sync_exits(s, symbol, events);
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
                   OrderType::Market, TimeInForce::Ioc, magnitude(q), {}, {}, {}, {}};
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
        if (!o.system && touches(o.request, symbol))
          cancel_order(o, failure(Reason::EXPIRY_CUTOFF, "Pre-expiry cutoff: the position is being closed"), events);
      flatten(s, symbol, "expiry", events);
    }
  }
}
/// Accept or reject one new order; once accepted, arm it or match it at once.
CommandResult place(State& s, OrderRequest request, Timestamp time, const Decision& rejection, Events& events) {
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
  const auto symbols = order_symbols(stored.request);
  const auto& contract = s.contracts.at(symbols.front());
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
  match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
  auto& accepted = s.orders.at(static_cast<std::size_t>(id - 1));
  if (accepted.open() && accepted.request.tif == TimeInForce::Ioc)
    cancel_order(accepted, failure(Reason::IOC_REMAINDER, "IOC exhausted available displayed liquidity"), events);
  return CommandResult{{}, id, 0};
}
std::string underlying_of(const State& s, const Order& o) {
  const auto c = s.contracts.find(order_symbols(o.request).front());
  return c == s.contracts.end() ? std::string{} : c->second.underlying;
}
/// Apply new terms to a resting order, or leave it untouched with the reason.
CommandResult change_order(State& s, OrderId id, const OrderChange& change, const Decision& rejection, Events& events) {
  if (id == 0 || id > s.orders.size()) return {failure(Reason::UNKNOWN_ORDER, "Unknown order ID"), {}, 0};
  auto& order = s.orders.at(static_cast<std::size_t>(id - 1));
  if (!order.open()) return {failure(Reason::ORDER_TERMINAL, "Order is already terminal"), id, 0};
  const auto& r = order.request;
  const bool exit = order.role != OrderRole::Normal;
  const bool resting = order.status == OrderStatus::Armed || (r.type == OrderType::Limit && r.tif == TimeInForce::Day);
  if (order.system || !resting)
    return {failure(Reason::INVALID_ORDER, "Only resting orders change: DAY limit orders, armed orders and bracket exits"), id, 0};
  if (change.empty()) return {failure(Reason::INVALID_ORDER, "Give a new quantity, limit price or trigger level"), id, 0};
  if (change.quantity && exit)
    return {failure(Reason::INVALID_ORDER, "A bracket exit's size follows its position; change its level or price instead"), id, 0};
  if (change.quantity && *change.quantity <= order.filled_quantity)
    return {{Reason::INVALID_ORDER, "The new quantity must exceed the filled quantity; cancel the order instead",
             static_cast<double>(*change.quantity), static_cast<double>(order.filled_quantity), {}}, id, 0};
  if (change.limit_price && r.type != OrderType::Limit)
    return {failure(Reason::INVALID_ORDER, "Only limit orders have a limit price"), id, 0};
  if (change.trigger_level && (!r.trigger || order.status != OrderStatus::Armed))
    return {failure(Reason::INVALID_ORDER, "Only armed orders with a trigger have a trigger level"), id, 0};
  if (!rejection.ok()) return {rejection, id, 0};
  const Order before = order;
  if (change.quantity) order.request.quantity = *change.quantity;
  if (change.limit_price) order.request.limit_price = *change.limit_price;
  if (change.trigger_level) order.request.trigger->level = *change.trigger_level;
  Decision decision;
  if (exit) {
    // Exits only ever reduce a position, so they skip the entry checks; their terms must still be valid.
    const auto& root = s.contracts.at(order.request.symbol).root;
    const auto& limit = order.request.limit_price;
    if ((order.request.trigger && order.request.trigger->level <= Money{}) || (limit && *limit <= Money{}))
      decision = failure(Reason::INVALID_ORDER, "Trigger levels and exit prices must be positive");
    else if (limit && limit->micros() % tick_size(root, *limit).micros() != 0)
      decision = failure(Reason::INVALID_TICK, "Take-profit price is not a positive multiple of the product tier tick");
  } else {
    decision = order_check(s, order);
  }
  if (!decision.ok()) {
    order = before;
    return {decision, id, 0};
  }
  event(events, "order_modified", order);
  const auto symbols = order_symbols(order.request);
  if (order.status == OrderStatus::Armed) {
    if (md::trading_session(s.contracts.at(symbols.front()).root, s.time).name == "regular" && reached(s, order))
      activate(s, id, events);
  } else {
    match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
  }
  return {{}, id, 0};
}
void require_reason(const std::string& reason) {
  if (reason.find_first_not_of(" \t\r\n") == std::string::npos)
    throw TradingError(Reason::INVALID_REASON, "An explicit nonblank reason is required");
}

constexpr std::uint64_t kCheckpointEvery = 1000;
/// Keeps schema 3 records small: each carries its state as the change from
/// the record before ("delta") or whole, as a checkpoint ("state"). A journal
/// starts with a checkpoint and has one every kCheckpointEvery records, and
/// any change more than half the size of the last checkpoint is recorded whole.
class StateRecorder {
 public:
  void add(Json& payload, Json state) {
    if (base_ && since_ + 1 < kCheckpointEvery) {
      auto delta = detail::state_delta(*base_, state);
      if (delta.dump().size() * 2 <= checkpoint_bytes_) {
        payload["delta"] = std::move(delta);
        base_ = std::move(state);
        ++since_;
        return;
      }
    }
    checkpoint_bytes_ = state.dump().size();
    payload["state"] = state;
    base_ = std::move(state);
    since_ = 0;
  }
 private:
  std::optional<Json> base_;
  std::uint64_t since_ = 0;
  std::size_t checkpoint_bytes_ = 0;
};

/// Reverify even caller-constructed recovery objects instead of trusting them.
JournalRecovery reverify(const JournalRecovery& recovery) {
  if (recovery.records.empty()) throw TradingError(Reason::JOURNAL_CORRUPT, "Recovery requires a session_start record");
  std::string lines;
  try {
    for (const auto& r : recovery.records) {
      lines += Json{{"seq", r.seq}, {"time", r.time}, {"type", r.type}, {"payload", Json::parse(r.payload)},
                    {"prev_hash", r.prev_hash}, {"hash", r.hash}}.dump() + '\n';
    }
  } catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
  auto verified = verify_journal(lines, recovery.head);
  if (verified.records.front().type != "session_start") throw TradingError(Reason::JOURNAL_CORRUPT, "Missing session start");
  return verified;
}

/// Visit each transaction of a verified journal in order with the whole state
/// it left: read from the record (schemas 1 and 2, and schema 3 checkpoints) or
/// rebuilt by applying the record's delta. Each state must carry its record's
/// sequence and time, and each new config passes validation. The visitor gets
/// the payload without its state; returns the last state.
template <class Visit>
Json walk_states(const std::vector<JournalRecord>& records, Visit&& visit) {
  Json state;
  bool chained = false;  // A delta may follow only a schema 3 record.
  Json validated;
  for (const auto& r : records) {
    auto payload = Json::parse(r.payload);
    const auto schema = payload.at("schema");
    const auto& ticks = payload.at("tick_policy");
    if ((schema != 1 && schema != 2 && schema != 3) || (ticks != "index-v1" && ticks != "v2"))
      throw TradingError(Reason::JOURNAL_CORRUPT, "Unsupported trading journal schema/policy");
    const auto whole = payload.find("state");
    if (schema == 3) {
      const auto delta = payload.find("delta");
      if ((whole == payload.end()) == (delta == payload.end()))
        throw TradingError(Reason::JOURNAL_CORRUPT, "A schema 3 record carries either a whole state or a delta");
      if (delta != payload.end()) {
        if (!chained) throw TradingError(Reason::JOURNAL_CORRUPT, "A state delta without a checkpoint before it");
        try { detail::apply_state_delta(state, *delta); }
        catch (const std::invalid_argument& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
      }
    } else {
      const auto& snapshot = payload.at("snapshot");
      if (snapshot.at("account_version") != r.seq || snapshot.at("time") != r.time)
        throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded snapshot version/time does not match transaction");
      if (whole == payload.end()) throw TradingError(Reason::JOURNAL_CORRUPT, "Record without its state");
    }
    if (whole != payload.end()) {
      state = std::move(*whole);
      payload.erase(whole);
    }
    chained = schema == 3;
    if (!state.is_object() || state.at("version") != r.seq || state.at("time") != r.time)
      throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded state version/time does not match transaction");
    if (const auto& config = state.at("config"); config != validated) {
      const auto c = config.get<SessionConfig>();
      validate_limits(c.limits);
      validate_scenarios(c.scenarios);
      validate_rules(c.rules);
      validated = config;
    }
    visit(r, std::move(payload), std::as_const(state));
  }
  return state;
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
  /// A new session's first record, and the first after recovery, are checkpoints.
  StateRecorder recorder;
  /// Whether moving the clock alone to `time` could change anything. Flat with
  /// no open orders, and once the attempt has its start time, time drives no
  /// rule: no DAY or expiry cancellation, trigger, mark, freshness flag, loss
  /// or evaluation change. Rollover is its own command.
  bool idle(Timestamp time) const {
    const auto& s = state;
    return !stopped && time >= s.time && s.ledger.positions().empty() &&
           s.evaluation.started > 0 && s.evaluation.cycle_started > 0 &&
           std::none_of(s.orders.begin(), s.orders.end(), [](const Order& o) { return o.open(); });
  }

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
      // Schema 3 records the state as a change from the record before, with
      // checkpoints, and no snapshot: recovery derives it from the state.
      // Tick policy v2 extends index-v1 with equity and ETF classes.
      Json payload{{"schema", 3}, {"tick_policy", "v2"}, {"events", events}, {"decision", result.decision}};
      recorder.add(payload, next);
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
    return place(s, std::move(request), time, rejection, events);
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
CommandResult TradingSession::modify(OrderId id, OrderChange change, Timestamp time, Decision rejection) {
  return impl_->transact(time, "modify", [&](State& s, Events& events) {
    monitor_loss(s, events);
    return change_order(s, id, change, rejection, events);
  });
}
CommandResult TradingSession::cancel_all(std::optional<std::string> underlying, Timestamp time) {
  return impl_->transact(time, "cancel_all", [&](State& s, Events& events) {
    for (auto& o : s.orders)
      if (o.open() && (!underlying || underlying_of(s, o) == *underlying))
        cancel_order(o, failure(Reason::USER_CANCEL, "Cancelled by caller"), events);
    return CommandResult{};
  });
}
CommandResult TradingSession::close_positions(std::optional<std::string> underlying, Timestamp time,
                                              const std::map<std::string, Decision>& rejections) {
  return impl_->transact(time, "close_positions", [&](State& s, Events& events) {
    monitor_loss(s, events);
    const auto in_scope = [&](const std::string& name) { return !underlying || name == *underlying; };
    for (auto& o : s.orders)
      if (o.open() && in_scope(underlying_of(s, o)))
        cancel_order(o, failure(Reason::USER_CANCEL, "Cancelled to close positions"), events);
    std::vector<std::pair<std::string, Quantity>> closing;
    // Expired contracts cannot trade; they close at settlement.
    for (const auto& [symbol, position] : s.ledger.positions()) {
      const auto& contract = s.contracts.at(symbol);
      if (position.quantity != 0 && in_scope(contract.underlying) && s.time < contract.expiry_time())
        closing.emplace_back(symbol, position.quantity);
    }
    // Shorts first: buying one back never uncovers another leg.
    std::stable_partition(closing.begin(), closing.end(), [](const auto& p) { return p.second < 0; });
    const auto prefix = "openport-close-" + std::to_string(s.version + 1) + "-";
    std::size_t count = 0;
    for (const auto& [symbol, quantity] : closing) {
      OrderRequest request;
      request.client_order_id = prefix + std::to_string(++count);
      request.symbol = symbol;
      request.side = quantity > 0 ? Side::Sell : Side::Buy;
      request.type = OrderType::Market;
      request.tif = TimeInForce::Ioc;
      request.quantity = magnitude(quantity);
      const auto gate = rejections.find(s.contracts.at(symbol).underlying);
      place(s, std::move(request), time, gate == rejections.end() ? Decision{} : gate->second, events);
    }
    return CommandResult{};
  });
}
CommandResult TradingSession::on_quotes(const std::vector<QuoteObservation>& quotes,
    const std::vector<Valuation>& valuations, Timestamp time) {
  // An empty batch on an idle account only moves the clock: not a transaction,
  // so nothing is journaled. The next transaction advances the clock itself.
  if (quotes.empty() && valuations.empty() && impl_->idle(time)) return CommandResult{{}, {}, impl_->state.version};
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
  const auto verified = reverify(recovery);
  if (journal && (recovery.truncated_final_line || journal->head() != verified.head || journal->sequence() != verified.records.size()))
    throw TradingError(Reason::JOURNAL_CORRUPT, "Recovery sink does not match verified journal head");
  auto impl = std::make_unique<Impl>();
  try {
    Json last;
    auto state = walk_states(verified.records, [&](const JournalRecord&, Json&& payload, const Json&) { last = std::move(payload); });
    impl->state = state.get<State>();
    // Schema 3 records no snapshot; the reducer derives it from the state.
    impl->snapshot = std::make_shared<TradingSnapshot>(
        last.at("schema") == 3 ? snapshot_of(impl->state) : last.at("snapshot").get<TradingSnapshot>());
    if (last.at("schema") == 1) {
      // A schema 1 account has no rules; start its progress record from the
      // journal's first transaction so later records continue it.
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
std::string TradingSession::compact(const JournalRecovery& recovery, Journal& out) {
  if (out.sequence() != 0) throw TradingError(Reason::JOURNAL_IO, "Compaction writes into an empty journal");
  const auto verified = reverify(recovery);
  (void)recover(recovery);  // Only a journal that recovers is rewritten.
  StateRecorder recorder;
  Json reread;
  try {
    const auto final_seq = verified.records.size();
    const auto last = walk_states(verified.records, [&](const JournalRecord& r, Json&& payload, const Json& state) {
      if (r.seq == final_seq && payload.at("schema") == 1) {
        // Recovery completes the evaluation of a journal whose last record is
        // schema 1 from that record, so it stays as it is.
        out.append(r.time, r.type, r.payload);
        return;
      }
      payload["schema"] = 3;
      payload.erase("snapshot");
      payload.erase("delta");
      recorder.add(payload, state);
      const auto line = payload.dump();
      // Read the record back as recovery will, before it is written.
      const auto check = Json::parse(line);
      if (const auto delta = check.find("delta"); delta != check.end()) detail::apply_state_delta(reread, *delta);
      else reread = check.at("state");
      if (reread != state)
        throw TradingError(Reason::JOURNAL_CORRUPT, "Compaction would change transaction " + std::to_string(r.seq));
      out.append(r.time, r.type, line);
    });
    if (Json::parse(verified.records.back().payload).at("schema") == 1) return recover(recovery).snapshot_json();
    return Json(snapshot_of(last.get<State>())).dump();
  } catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
    catch (const std::invalid_argument& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
}
void TradingSession::expand(const JournalRecovery& recovery, Journal& out) {
  if (out.sequence() != 0) throw TradingError(Reason::JOURNAL_IO, "Expansion writes into an empty journal");
  const auto verified = reverify(recovery);
  (void)recover(recovery);
  try {
    (void)walk_states(verified.records, [&](const JournalRecord& r, Json&& payload, const Json& state) {
      if (payload.at("schema") != 3) { out.append(r.time, r.type, r.payload); return; }
      payload["schema"] = 2;
      payload.erase("delta");
      payload["snapshot"] = snapshot_of(state.get<State>());
      payload["state"] = state;
      out.append(r.time, r.type, payload.dump());
    });
  } catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
}
}  // namespace openport::trading

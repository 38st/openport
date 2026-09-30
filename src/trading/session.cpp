#include "openport/trading/session.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

#include "openport/trading/history.hpp"
#include "state.hpp"
#include "review.hpp"
#include "state_delta.hpp"

namespace openport::trading {
namespace {
using detail::State;
using Events = std::vector<Json>;
Decision failure(Reason code, std::string message) { return {code, std::move(message), {}, {}, {}}; }
/// Appends an order; its client order ID keeps naming the first order that used it.
void add_order(State& s, const Order& order) {
  s.clients.emplace(order.request.client_order_id, order.id);
  s.orders.push_back(order);
}
void event(Events& events, std::string_view type, Json payload) {
  events.push_back(Json{{"type", type}, {"payload", std::move(payload)}});
}
bool regular(const md::OptionContract& c, Timestamp time) { return md::trading_session(c.root, time).name == "regular"; }
/// Whether the contract trades now, and the order with it: every order in the
/// regular session; plain limit orders in the overnight (global) and curb sessions.
Decision session_check(const md::OptionContract& c, Timestamp time, const OrderRequest& r) {
  if (time >= c.last_trade_time())
    return failure(Reason::SESSION_CLOSED, "AM-settled series stop trading at the regular close before expiry");
  if (r.tif == TimeInForce::Gtc && r.type == OrderType::Limit) return {};
  const auto session = md::trading_session(c.root, time);
  if (!session.open) return failure(Reason::SESSION_CLOSED, "Outside the contract's trading sessions");
  if (session.name != "regular" && (r.type != OrderType::Limit || r.trigger || r.bracket))
    return failure(Reason::LIMIT_ONLY, "The overnight and curb sessions take plain limit orders only: "
                   "no market orders, triggers or brackets");
  return {};
}
/// A DAY order lasts the session it was accepted in.
Timestamp session_end(const md::OptionContract& c, Timestamp time) {
  const auto session = md::trading_session(c.root, time);
  if (!session.open || session.end == md::kInvalidTimestamp)
    throw TradingError(Reason::SESSION_CLOSED, "No representable session end");
  return session.end;
}
/// Persistent orders stop at the first leg's last trade or account auto-close.
Timestamp order_expiry(const State& s, const OrderRequest& r) {
  auto end = std::numeric_limits<Timestamp>::max();
  for (const auto& symbol : order_symbols(r))
    end = std::min(end, s.contracts.at(symbol).last_trade_time() - s.config.rules.expiry_cutoff);
  return end;
}
bool persistent(const Order& o) {
  return o.request.tif == TimeInForce::Gtc || o.request.trigger || o.role != OrderRole::Normal;
}
Annotation clean_annotation(std::string note, const std::vector<std::string>& tags);
void store_annotation(State& s, const std::string& key, Annotation annotation, Events& events);
Decision system_check(const State& s, const Order& o);
Money mid(const QuoteObservation& q) {
  // Both sides are positive. Difference-first avoids overflowing their sum.
  return *q.bid + (*q.ask - *q.bid).prorate(1, 2);
}
/// A markable quote's mark: its mid, or halfway to the ask when nobody bids.
Money mark_of(const QuoteObservation& q) { return q.bid ? mid(q) : q.ask->prorate(1, 2); }
Decision quote_check(const State& s, const std::string& symbol) {
  const auto it = s.books.find(symbol);
  if (it == s.books.end() || !valid_quote(it->second.quote))
    return failure(Reason::INVALID_QUOTE, "A noncrossed positive two-sided quote with both sizes is required");
  const auto time = it->second.quote.time;
  if (time > s.time || observation_time(s.contracts.at(symbol), s.time) - time > s.config.limits.max_quote_age)
    return failure(Reason::STALE_QUOTE, "Quote is outside the configured market-time freshness window");
  return {};
}
/// Whether a position's quote marks it now: markable, and inside the freshness window.
/// A far option nobody bids for is marked; only a valid quote trades.
bool marked_now(const State& s, const std::string& symbol) {
  const auto it = s.books.find(symbol);
  if (it == s.books.end() || !markable_quote(it->second.quote)) return false;
  const auto time = it->second.quote.time;
  return time <= s.time && observation_time(s.contracts.at(symbol), s.time) - time <= s.config.limits.max_quote_age;
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
const Valuation* valuation_of(const State& s, const std::string& symbol) {
  const auto it = s.valuations.find(symbol);
  return it == s.valuations.end() || !valid_valuation(it->second) ? nullptr : &it->second;
}
/// A stretch's change in value to `value` per unit, split by the Greeks at its
/// start; without valuations at both ends, it is all `other`.
Attribution explain(const md::OptionContract& c, const detail::Reference& r, Money value, const Valuation* now) {
  Attribution a;
  const double size = static_cast<double>(r.quantity) * c.multiplier;
  if (r.valuation && now) {
    const auto& v = *r.valuation;
    const double move = now->spot - v.spot;
    a.delta = size * v.delta * move;
    a.gamma = size * 0.5 * v.gamma * move * move;
    a.vega = size * v.vega * (now->smile_iv - v.smile_iv) * 100;
    a.theta = size * v.theta * (v.years - now->years) * 365;
  }
  a.other = size * (value - r.mark).dollars() - a.delta - a.gamma - a.vega - a.theta;
  return a;
}
/// End a held contract's stretch at `value` per unit, into today's explanation.
void end_stretch(State& s, const std::string& symbol, Money value, const Valuation* now) {
  const auto it = s.references.find(symbol);
  if (it == s.references.end()) return;
  s.explained[symbol] += explain(s.contracts.at(symbol), it->second, value, now);
  s.references.erase(it);
}
/// Start a stretch at the contract's size, mark and valuation now.
void start_stretch(State& s, const std::string& symbol) {
  const auto quantity = held(s, symbol);
  const auto mark = s.marks.find(symbol);
  if (quantity == 0 || mark == s.marks.end()) return;
  const auto* valuation = valuation_of(s, symbol);
  s.references[symbol] = {quantity, mark->second.price, valuation ? std::optional(*valuation) : std::nullopt};
}
/// Book a fill: the stretch before it ends at the mark, the spread paid against
/// the mark and the fee are costs, and a stretch at the new size starts.
void fill_position(State& s, const std::string& symbol, Quantity signed_quantity, Money price, Money fee) {
  const auto& contract = s.contracts.at(symbol);
  const auto mark = s.marks.find(symbol);
  const Money value = mark == s.marks.end() ? price : mark->second.price;
  end_stretch(s, symbol, value, valuation_of(s, symbol));
  s.ledger.fill(contract, signed_quantity, price, fee);
  s.explained[symbol].costs += static_cast<double>(signed_quantity) * contract.multiplier * (value - price).dollars() - fee.dollars();
  start_stretch(s, symbol);
}
/// American equity and ETF options deliver shares; index options settle in cash.
bool physical(const md::OptionContract& c) {
  return c.style == pricing::ExerciseStyle::American && !md::is_index_underlying(c.underlying);
}
Quantity shares_held(const State& s, const std::string& symbol) {
  const auto it = s.ledger.stocks().find(symbol);
  return it == s.ledger.stocks().end() ? 0 : it->second.shares;
}
/// The underlying's price if fresh: within max_quote_age of the market time
/// while the stock market is open, or of its last close while it is not (the
/// 16:00 close stays current while the options trade on to 16:15, and overnight).
std::optional<Money> stock_price(const State& s, const std::string& symbol) {
  const auto it = s.stock_marks.find(symbol);
  if (it == s.stock_marks.end() || it->second.time > s.time) return std::nullopt;
  const auto session = md::stock_session(s.time);
  const auto observed = session.open || session.market_time == md::kInvalidTimestamp ? s.time : std::min(s.time, session.market_time);
  if (observed - it->second.time > s.config.limits.max_quote_age) return std::nullopt;
  return it->second.price;
}
/// Shares' stretches are all delta: the price's move times the shares.
Attribution explain_stock(const detail::Reference& r, Money value) {
  Attribution a;
  a.delta = static_cast<double>(r.quantity) * (value - r.mark).dollars();
  return a;
}
void start_stock_stretch(State& s, const std::string& symbol) {
  const auto shares = shares_held(s, symbol);
  const auto mark = s.stock_marks.find(symbol);
  if (shares == 0 || mark == s.stock_marks.end()) return;
  s.references[symbol] = {shares, mark->second.price, std::nullopt};
}
/// Trade shares at `price`: the stretch before ends at the underlying's mark,
/// any difference from it is a cost, and a stretch at the new size starts. The
/// fill goes into the trade history with how it came about.
void trade_shares(State& s, const std::string& symbol, Quantity signed_shares, Money price, StockSource source,
                  const std::string& option = {}) {
  const auto mark = s.stock_marks.find(symbol);
  const Money value = mark == s.stock_marks.end() ? price : mark->second.price;
  if (const auto it = s.references.find(symbol); it != s.references.end()) {
    s.explained[symbol] += explain_stock(it->second, value);
    s.references.erase(it);
  }
  s.ledger.trade_stock(symbol, signed_shares, price, Money{});
  s.explained[symbol].costs += static_cast<double>(signed_shares) * (value - price).dollars();
  s.stock_fills.push_back({s.stock_fills.size() + 1, symbol, signed_shares, price, s.time, source, option});
  start_stock_stretch(s, symbol);
}
/// Shares an option position becomes: 100 a contract, bought by long calls and
/// short puts, sold by long puts and short calls.
Quantity delivered(const md::OptionContract& c, Quantity contracts) {
  return (c.type == pricing::OptionType::Call ? 1 : -1) * static_cast<Quantity>(c.multiplier) * contracts;
}
/// A bracket's two exits can fill only once between them: checks count the pair
/// through its earlier order, so the later one is shadowed while both are open.
bool shadowed(const State& s, const Order& o) {
  return o.oco != 0 && o.oco < o.id && s.orders.at(static_cast<std::size_t>(o.oco - 1)).open();
}
/// Every leg must oppose its holding and fit within it after the other working
/// user orders on that side. Bracket exits shrink after each fill and system
/// closes are immediate IOC, so neither reserves a manual close's capacity.
bool closing_only(const State& s, const Order& o, bool include_working = true) {
  const auto closes = [&](const std::string& symbol, Side side, Quantity ratio) {
    const auto q = held(s, symbol);
    if (q == 0 || (side != Side::Buy && side != Side::Sell) ||
        (q > 0) == (side == Side::Buy) || o.remaining() <= 0 || ratio < 1 || ratio > kMaxRatio)
      return false;
    auto capacity = magnitude(q);
    const auto reserve = [&](Quantity units, Quantity weight) {
      if (units > capacity / weight) return false;
      capacity -= units * weight;
      return true;
    };
    if (!reserve(o.remaining(), ratio)) return false;
    if (!include_working) return true;
    for (const auto id : open_ids(s)) {
      const auto& other = s.orders[id - 1];
      if (other.system || other.role != OrderRole::Normal || other.id == o.id) continue;
      if (multi_leg(other.request)) {
        for (const auto& leg : other.request.legs)
          if (leg.symbol == symbol && leg.side == side && !reserve(other.remaining(), leg.ratio)) return false;
      } else if (other.request.symbol == symbol && other.request.side == side && !reserve(other.remaining(), 1)) {
        return false;
      }
    }
    return true;
  };
  if (multi_leg(o.request))
    return std::all_of(o.request.legs.begin(), o.request.legs.end(), [&](const Leg& leg) {
      return closes(leg.symbol, leg.side, leg.ratio);
    });
  return closes(o.request.symbol, o.request.side, 1);
}
bool opens(Quantity held_quantity, Quantity signed_fill) {
  return held_quantity == 0 || (held_quantity > 0) == (signed_fill > 0) || magnitude(signed_fill) > magnitude(held_quantity);
}
/// Whether an order trades `symbol`, as its contract or one of its legs.
bool touches(const OrderRequest& r, const std::string& symbol) {
  return r.symbol == symbol || std::any_of(r.legs.begin(), r.legs.end(), [&](const Leg& leg) { return leg.symbol == symbol; });
}
Quantity signed_contracts(const Leg& leg, Quantity units) { return leg.side == Side::Buy ? units * leg.ratio : -units * leg.ratio; }
Quantity depth_used(Quantity size, Quantity left, Quantity offset = 0) {
  Quantity used = 0;
  if (__builtin_sub_overflow(size, left, &used) || __builtin_add_overflow(used, offset, &used))
    throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Simulated depth exceeds quantity range");
  return used;
}
void consume_depth(Quantity& left, Quantity quantity) {
  if (__builtin_sub_overflow(left, quantity, &left))
    throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Liquidity budget exceeds quantity range");
}
/// Slippage and simulated depth use the displayed price's tick tier. A negative
/// budget carries depth consumed by earlier orders on this same observation.
Money execution_price(const State& s, const std::string& symbol, Side side, std::optional<Money> limit = {}, Quantity offset = 0) {
  const auto& book = s.books.at(symbol);
  const auto& quote = book.quote;
  const bool buy = side == Side::Buy;
  const auto displayed = buy ? *quote.ask : *quote.bid;
  const auto size = buy ? quote.ask_size : quote.bid_size;
  const auto left = buy ? book.ask_left : book.bid_left;
  const auto blocks = s.config.rules.impact_ticks > 0 ? depth_used(size, left, offset) / size : 0;
  const auto tick = tick_size(s.contracts.at(symbol).root, displayed);
  const auto slip = tick * s.config.rules.slippage_ticks + tick * s.config.rules.impact_ticks * blocks;
  const auto price = buy ? displayed + slip : std::max(Money{}, displayed - slip);
  return limit && s.config.rules.impact_ticks == 0 ? (buy ? std::min(*limit, price) : std::max(*limit, price)) : price;
}
struct ExecutionSlice {
  std::string symbol;
  Side side;
  Quantity quantity;
  Money price;
};
/// Impact combos execute one whole unit at a time. A ratio can span depth tiers;
/// keep those prices separate so both the ledger and the net stay exact.
std::vector<ExecutionSlice> combo_slices(const State& s, const OrderRequest& r, Quantity units) {
  std::vector<ExecutionSlice> slices;
  for (const auto& leg : r.legs) {
    if (s.config.rules.impact_ticks == 0) {
      slices.push_back({leg.symbol, leg.side, units * leg.ratio, execution_price(s, leg.symbol, leg.side)});
    } else {
      for (Quantity offset = 0; offset < units * leg.ratio; ++offset) {
        const auto price = execution_price(s, leg.symbol, leg.side, {}, offset);
        if (!slices.empty() && slices.back().symbol == leg.symbol && slices.back().price == price) ++slices.back().quantity;
        else slices.push_back({leg.symbol, leg.side, 1, price});
      }
    }
  }
  return slices;
}
/// A multi-leg order's next executable net debit per unit.
std::optional<Money> executable_net(const State& s, const OrderRequest& r) {
  for (const auto& leg : r.legs) {
    const auto book = s.books.find(leg.symbol);
    if (book == s.books.end() || !valid_quote(book->second.quote)) return std::nullopt;
  }
  Money net;
  for (const auto& leg : r.legs) {
    if (s.config.rules.impact_ticks == 0) {
      const auto price = execution_price(s, leg.symbol, leg.side) * leg.ratio;
      net = leg.side == Side::Buy ? net + price : net - price;
    } else {
      for (Quantity offset = 0; offset < leg.ratio; ++offset) {
        const auto price = execution_price(s, leg.symbol, leg.side, {}, offset);
        net = leg.side == Side::Buy ? net + price : net - price;
      }
    }
  }
  return net;
}
/// Quote time, not transaction time alone, must reach the delay. Reconfirmed
/// snapshots may qualify but do not replenish the shared liquidity budget.
bool latency_ready(const State& s, const Order& o) {
  if (s.config.rules.fill_latency_ms == 0) return true;
  const auto start = std::max(o.accepted_at, o.triggered_at);
  const auto delay = s.config.rules.fill_latency_ms * (md::kNanosPerSecond / 1000);
  for (const auto& symbol : order_symbols(o.request)) {
    const auto& quote = s.books.at(symbol).quote;
    if (quote.time < start || quote.time - start < delay || !quote_check(s, symbol).ok()) return false;
    if ((persistent(o) || o.system) && !regular(s.contracts.at(symbol), s.time)) return false;
  }
  return true;
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
/// Short shares hold 150% of their value, as a short sale does; long shares are paid for.
Money stock_requirement(const State& s) {
  Money total;
  for (const auto& [symbol, stock] : s.ledger.stocks()) {
    if (stock.shares >= 0) continue;
    const auto mark = s.stock_marks.find(symbol);
    const Money value = mark == s.stock_marks.end() ? -stock.basis : mark->second.price * -stock.shares;
    total = total + value.prorate(3, 2);
  }
  return total;
}
struct Margin {
  Money requirement;  ///< What the book requires, as reported.
  Money held;         ///< What it holds out of cash: the requirement less the positions' value under portfolio margin.
};
/// The positions' signed value at their marks (shorts negative), shares included;
/// a short without a mark counts at its buy-back value and a long without one at nothing.
Money position_value(const State& s, const MarginBook& book) {
  Money value;
  for (const auto& [symbol, entry] : book) {
    const auto mark = s.marks.find(symbol);
    if (mark != s.marks.end()) value = value + (mark->second.price * 100) * entry.first;
    else if (entry.first < 0) value = value - entry.second;
  }
  for (const auto& [symbol, stock] : s.ledger.stocks())
    if (const auto price = stock_price(s, symbol)) value = value + *price * stock.shares;
  return value;
}
Margin margin_of(const State& s, const MarginBook& book) {
  std::vector<MarginLeg> legs;
  for (const auto& [symbol, entry] : book) legs.push_back({s.contracts.at(symbol), entry.first, entry.second, spot_for(s, symbol)});
  Money minimum;
  if (s.config.rules.margin == MarginMode::Portfolio) {
    std::map<std::string, double> prices;
    for (const auto& [symbol, stock] : s.ledger.stocks())
      if (const auto price = stock_price(s, symbol)) prices[symbol] = price->dollars();
    // Portfolio margin is taken from equity: cash already holds shorts' credits and
    // paid for longs, so the positions' value is what they are worth on top of it.
    if (const auto scanned = portfolio_margin_requirement(legs, s.valuations, s.time,
        s.config.limits.max_valuation_age, s.ledger.stocks(), prices))
      return {*scanned, *scanned - position_value(s, book)};
    // An incomplete scan falls back to strategy margin plus the option minimum.
    // The snapshot flags missing data and user fills require fresh valuations.
    for (const auto& leg : legs)
      minimum = minimum + Money::from_double(0.375 * leg.contract.multiplier) * magnitude(leg.quantity);
  }
  const auto strategy = margin_requirement(legs) + stock_requirement(s) + minimum;
  return {strategy, strategy};
}
Money requirement(const State& s, const MarginBook& book) { return margin_of(s, book).held; }
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
  const auto margin = margin_of(s, book);
  std::map<std::string, std::pair<Quantity, Quantity>> capacity;
  Money reserved;
  for (const auto id : open_ids(s)) {
    const auto& o = s.orders[id - 1];
    if (o.remaining() <= 0 || shadowed(s, o)) continue;
    const auto remaining = o.remaining();
    Use use;
    Quantity opening = 0;
    if (multi_leg(o.request)) {
      if (o.role != OrderRole::Normal) {
        Money fees;
        for (const auto& leg : o.request.legs) fees = fees + s.config.fee_per_contract * (remaining * leg.ratio);
        use = {fees, false};
      } else use = combo_use(s, o, book);
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
        price = execution_price(s, symbol, o.request.side);
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
  out.total.short_requirement = margin.requirement;
  out.total.available = s.ledger.account().cash - margin.held - reserved;
  return out;
}
/// The personal soft floor now: an absolute level, or a share of the plan's
/// drawdown above its floor, whichever is higher.
std::optional<Money> soft_floor_of(const State& s) {
  std::optional<Money> floor;
  const auto& personal = s.config.guardrails;
  if (personal.soft_floor > Money{}) floor = personal.soft_floor;
  if (personal.soft_floor_percent > 0 && s.config.rules.max_drawdown > Money{}) {
    const auto level = s.evaluation.floor + s.config.rules.max_drawdown.prorate(personal.soft_floor_percent, 100);
    floor = floor ? std::max(*floor, level) : level;
  }
  return floor;
}
/// What rules and checks read of the account now: its equity at the marks and
/// the positions behind it, risk, the soft floor and buying power. Unlike a
/// snapshot it copies no history and builds no scenario grid or attribution,
/// so it costs the open positions and orders, not the account's past.
struct Measures {
  Money equity;
  Money unrealised;
  bool valuation_complete = true;
  std::vector<MarkedPosition> positions;
  std::vector<MarkedStock> stocks;
  std::map<std::string, double> stock_prices;
  RiskSnapshot risk;
  std::optional<Money> soft_floor;
  BuyingPower buying_power;
};
Measures measure(const State& s) {
  Measures out;
  out.equity = s.ledger.account().cash;
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
      p.fresh = !p.awaiting_settlement && marked_now(s, symbol);
      out.equity = out.equity + *p.market_value;
      out.unrealised = out.unrealised + *p.unrealised;
    }
    out.valuation_complete &= p.fresh;
    out.positions.push_back(std::move(p));
  }
  for (const auto& [symbol, stock] : s.ledger.stocks()) {
    MarkedStock m;
    m.position = stock;
    if (const auto it = s.stock_marks.find(symbol); it != s.stock_marks.end()) {
      m.mark = it->second.price;
      m.mark_time = it->second.time;
      m.market_value = it->second.price * stock.shares;
      m.unrealised = *m.market_value - stock.basis;
      m.fresh = stock_price(s, symbol).has_value();
      out.equity = out.equity + *m.market_value;
      out.unrealised = out.unrealised + *m.unrealised;
    }
    out.valuation_complete &= m.fresh;
    out.stocks.push_back(std::move(m));
  }
  for (const auto& [symbol, stock] : s.ledger.stocks())
    if (const auto price = stock_price(s, symbol)) out.stock_prices[symbol] = price->dollars();
  // Reachable exposure counts each open bracket pair once: the pair's later exit
  // is left out while both are open.
  std::vector<Order> working;
  for (const auto id : open_ids(s))
    if (const auto& o = s.orders[id - 1]; !shadowed(s, o)) working.push_back(o);
  out.risk = portfolio_risk(s.ledger, working, s.contracts, s.valuations, s.config.limits, s.time, out.stock_prices);
  out.risk.daily_loss = std::max(Money{}, s.start_equity - out.equity);
  out.risk.kill_latched = s.kill;
  out.risk.kill_reason = s.kill_reason;
  out.risk.limits_revision = s.limits_revision;
  out.soft_floor = soft_floor_of(s);
  out.buying_power = buying_power(s).total;
  return out;
}
TradingSnapshot snapshot_of(const State& s) {
  auto m = measure(s);
  TradingSnapshot out;
  out.account_version = s.version;
  out.time = s.time;
  out.account = s.ledger.account();
  out.equity = m.equity;
  out.start_of_day_equity = s.start_equity;
  out.unrealised = m.unrealised;
  out.valuation_complete = m.valuation_complete;
  out.recent_orders = s.orders;
  out.recent_fills = s.fills;
  for (const auto id : open_ids(s)) out.open_orders.push_back(s.orders[id - 1]);
  out.positions = std::move(m.positions);
  out.stocks = std::move(m.stocks);
  out.risk = std::move(m.risk);
  out.scenarios = scenario_grid(s.ledger, s.valuations, s.config.scenarios, s.time, s.config.limits.max_valuation_age, m.stock_prices);
  if (!out.valuation_complete) out.quality_flags.push_back(Reason::STALE_QUOTE);
  if (!out.risk.complete || !out.scenarios.complete) out.quality_flags.push_back(Reason::MISSING_VALUATION);
  if (std::any_of(out.positions.begin(), out.positions.end(), [](const auto& p) { return p.awaiting_settlement; }))
    out.quality_flags.push_back(Reason::AWAITING_SETTLEMENT);
  out.evaluation = s.evaluation;
  out.pending_limits = s.pending_limits;
  out.pending_guardrails = s.pending_guardrails;
  out.guardrails = s.guardrails;
  out.pending_applied_at = s.pending_applied_at;
  out.soft_floor = m.soft_floor;
  out.buying_power = m.buying_power;
  out.closures = s.closures;
  out.attempts = s.attempts;
  out.stock_fills = s.stock_fills;
  out.dividends = s.dividends;
  out.closing_prints = s.closing_prints;
  out.annotations = s.annotations;
  out.day_notes = s.day_notes;
  out.trade_reviews = s.trade_reviews;
  out.strategy_reviews = s.strategy_reviews;
  // Today's P&L by Greek: the finished stretches, and the open ones to the marks now.
  out.attributions = s.explained;
  for (const auto& [symbol, reference] : s.references) {
    if (s.contracts.contains(symbol)) {
      if (const auto mark = s.marks.find(symbol); mark != s.marks.end())
        out.attributions[symbol] += explain(s.contracts.at(symbol), reference, mark->second.price, valuation_of(s, symbol));
    } else if (const auto mark = s.stock_marks.find(symbol); mark != s.stock_marks.end()) {
      out.attributions[symbol] += explain_stock(reference, mark->second.price);
    }
  }
  for (const auto& [symbol, attribution] : out.attributions) out.attribution += attribution;
  return out;
}
/// Rules only act on fully marked equity: every position has a mark, fresh or not.
template <class Account>
std::optional<Money> marked_equity(const Account& account) {
  for (const auto& p : account.positions) if (!p.market_value) return std::nullopt;
  for (const auto& p : account.stocks) if (!p.market_value) return std::nullopt;
  return account.equity;
}
/// All legs in an atomic execution share the account's pre-execution context.
FillContext fill_context(const State& s, const std::string& symbol, const Measures& snapshot) {
  FillContext context;
  if (const auto* v = valuation_of(s, symbol); v && s.time - v->time <= s.config.limits.max_valuation_age) {
    context.spot = v->spot;
    context.spot_source = v->spot_source;
    context.iv = v->smile_iv;
    context.delta = v->delta;
    context.years = v->years;
  } else if (const auto price = stock_price(s, s.contracts.at(symbol).underlying)) {
    context.spot = price->dollars();
    context.spot_source = "quote";
  }
  if (!context.years) context.years = static_cast<double>(std::max<Timestamp>(0, s.contracts.at(symbol).expiry_time() - s.time)) /
      static_cast<double>(md::kNanosPerDay) / 365.0;
  context.equity = marked_equity(snapshot);
  if (context.equity && s.config.rules.max_drawdown > Money{}) context.floor_room = *context.equity - s.evaluation.floor;
  context.buying_power = snapshot.buying_power.available;
  return context;
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
  e.floor = evaluation_floor(s.config.rules, e.peak, e.floor_locked);
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
bool personal_reason(std::string_view reason) {
  return reason == "SOFT_FLOOR" || reason == "TRADE_LIMIT" || reason == "COOLDOWN" || reason == "PROFIT_LOCK";
}
Reason guardrail_reason(const State& s) {
  if (!s.guardrails.latched.empty()) return s.guardrails.latched.front();
  return s.time < s.guardrails.cooldown_until ? Reason::COOLDOWN : Reason::NONE;
}
void trip(State& s, const std::string& reason, Events& events) {
  if (!s.kill || (s.guardrails.owns_kill && s.kill_reason != reason)) {
    s.kill = true;
    s.kill_reason = reason;
    s.guardrails.owns_kill = personal_reason(reason);
    event(events, "kill_trip", Json{{"reason", reason}});
  }
  // Remove opening orders first, so they cannot take a close's capacity; then any
  // closes that together exceed the position, newest first, so older ones keep priority.
  const auto cancel_unless_closing = [&](OrderId id, bool include_working) {
    const auto& o = s.orders[id - 1];
    if (o.open() && !o.system && o.role == OrderRole::Normal && !closing_only(s, o, include_working))
      cancel_order(s.orders.mut(id - 1), failure(Reason::KILL_SWITCH, s.kill_reason), events);
  };
  const auto ids = open_ids(s);
  for (const auto id : ids) cancel_unless_closing(id, false);
  for (auto it = ids.rbegin(); it != ids.rend(); ++it) cancel_unless_closing(*it, true);
}
Decision loss_check(const State& s, const Measures& snapshot) {
  if (snapshot.risk.daily_loss > s.config.limits.max_daily_loss)
    return {Reason::DAILY_LOSS, "Marked loss from start-of-day equity exceeds limit",
        snapshot.risk.daily_loss.dollars(), s.config.limits.max_daily_loss.dollars(), "aggregate"};
  return {};
}
void monitor_loss(State& s, Events& events) {
  if (!loss_check(s, measure(s)).ok()) trip(s, "DAILY_LOSS", events);
}
void latch_guardrail(State& s, Reason reason, Events& events) {
  auto& latched = s.guardrails.latched;
  if (reason != Reason::COOLDOWN && std::find(latched.begin(), latched.end(), reason) == latched.end()) {
    latched.push_back(reason);
    event(events, "guardrail_latched", Json{{"reason", reason}});
  }
  trip(s, std::string(to_string(guardrail_reason(s))), events);
}
void refresh_guardrail_latch(State& s, Events& events) {
  const auto reason = guardrail_reason(s);
  if (reason != Reason::NONE) trip(s, std::string(to_string(reason)), events);
  else if (s.kill && s.guardrails.owns_kill) {
    s.kill = false;
    s.kill_reason.clear();
    s.guardrails.owns_kill = false;
    event(events, "guardrail_released", Json::object());
  }
}
void begin_cooldown(State& s, Events& events) {
  if (s.config.guardrails.cooldown_minutes == 0) return;
  const auto duration = s.config.guardrails.cooldown_minutes * md::kNanosPerMinute;
  if (s.time > std::numeric_limits<Timestamp>::max() - duration)
    throw TradingError(Reason::INVALID_TIME, "Cooldown time exceeds timestamp range");
  s.guardrails.cooldown_until = std::max(s.guardrails.cooldown_until, s.time + duration);
  event(events, "guardrail_latched", Json{{"reason", Reason::COOLDOWN}, {"until", s.guardrails.cooldown_until}});
  latch_guardrail(s, Reason::COOLDOWN, events);
}
void guardrail_fill(State& s, OrderId id, bool opening, Money realised_before, Events& events) {
  if (opening) ++s.guardrails.opening_trades;
  const auto& g = s.config.guardrails;
  if (g.max_opening_trades > 0 && s.guardrails.opening_trades >= g.max_opening_trades)
    latch_guardrail(s, Reason::TRADE_LIMIT, events);
  if (s.orders.at(static_cast<std::size_t>(id - 1)).role == OrderRole::StopLoss ||
      (g.cooldown_loss > Money{} && s.ledger.account().realised - realised_before < -g.cooldown_loss))
    begin_cooldown(s, events);
}
void advance(State& s, Timestamp time, Events& events) {
  if (time < 0 || time < s.time) throw TradingError(Reason::INVALID_TIME, "Market time must be nonnegative and monotone");
  s.time = time;
  refresh_guardrail_latch(s, events);
  for (const auto id : open_ids(s)) {
    const auto& o = s.orders[id - 1];
    auto expiry = std::numeric_limits<Timestamp>::max();
    for (const auto& symbol : order_symbols(o.request)) expiry = std::min(expiry, s.contracts.at(symbol).expiry_time());
    if (persistent(o)) expiry = std::min(expiry, order_expiry(s, o.request));
    if (time >= expiry) cancel_order(s.orders.mut(id - 1), failure(Reason::EXPIRED, "A contract reached its last trade or auto-close deadline"), events);
    else if (time >= o.day_end) cancel_order(s.orders.mut(id - 1), failure(Reason::DAY_END, "The order's session ended"), events);
  }
}
/// Short contracts no long covers, after the account's positions take `extra`
/// (a projected order's signed contracts).
Quantity uncovered(const State& s, const std::vector<std::pair<std::string, Quantity>>& extra) {
  std::map<std::string, Quantity> held;
  for (const auto& [symbol, p] : s.ledger.positions()) held[symbol] = p.quantity;
  for (const auto& [symbol, q] : extra) held[symbol] += q;
  std::vector<MarginLeg> legs;
  for (const auto& [symbol, q] : held)
    if (q != 0) legs.push_back({s.contracts.at(symbol), q, {}, std::nullopt});
  return naked_shorts(legs);
}
/// A defined-risk plan refuses an order that would leave more shorts uncovered.
Decision defined_risk_check(const State& s, const std::vector<std::pair<std::string, Quantity>>& order,
                            std::string_view message = "This plan allows defined risk only: cover each short option with a "
                                                       "long of the same type that expires with it or later, or open the "
                                                       "spread as one order") {
  if (!s.config.rules.defined_risk || uncovered(s, order) <= uncovered(s, {})) return {};
  return failure(Reason::DEFINED_RISK, std::string(message));
}
/// Shorts left uncovered once every open order but `except` has sold all it
/// offers and bought nothing; a multi-leg order fills whole, and a bracket's two
/// exits sell its position once.
Quantity uncovered_if_sold(const State& s, OrderId except, std::vector<std::pair<std::string, Quantity>> extra) {
  for (const auto id : open_ids(s)) {
    const auto& o = s.orders[id - 1];
    if (o.id == except || shadowed(s, o)) continue;
    if (multi_leg(o.request)) {
      for (const auto& leg : o.request.legs) extra.emplace_back(leg.symbol, signed_contracts(leg, o.remaining()));
    } else if (o.request.side == Side::Sell) {
      extra.emplace_back(o.request.symbol, -o.remaining());
    }
  }
  return uncovered(s, extra);
}
/// A new or changed order must not leave more shorts uncovered with the open
/// orders filled either, so a working sell never takes the long a short needs.
Decision open_orders_risk_check(const State& s, const Order& o) {
  if (!s.config.rules.defined_risk) return {};
  std::vector<std::pair<std::string, Quantity>> order;
  if (multi_leg(o.request)) {
    for (const auto& leg : o.request.legs) order.emplace_back(leg.symbol, signed_contracts(leg, o.remaining()));
  } else {
    order.emplace_back(o.request.symbol, o.request.side == Side::Buy ? o.remaining() : -o.remaining());
  }
  if (uncovered_if_sold(s, o.id, order) <= uncovered_if_sold(s, o.id, {})) return {};
  return failure(Reason::DEFINED_RISK, "With your open orders filled, this would leave a short option uncovered: cancel "
                 "the order that sells its long, or that opens the short, first, or trade the spread as one order");
}
Decision account_check(const State& s, bool reducing = false) {
  if (s.kill && !reducing) {
    const auto personal = guardrail_reason(s);
    return failure(personal != Reason::NONE ? personal : Reason::KILL_SWITCH, s.kill_reason);
  }
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
      !r.client_order_id.empty() && r.quantity > 0 &&
      ((r.type == OrderType::Market && r.tif == TimeInForce::Ioc && !r.limit_price) ||
       (r.type == OrderType::Limit && r.limit_price && (r.tif == TimeInForce::Day || r.tif == TimeInForce::Ioc || r.tif == TimeInForce::Gtc)));
  if (!shape)
    return failure(Reason::INVALID_ORDER, "Multi-leg orders take two to four legs, a unit quantity and a net limit "
                   "(negative for a credit) or market IOC");
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
    if (auto d = session_check(c->second, s.time, r); !d.ok()) { d.scope = leg.symbol; return d; }
    if (r.quantity > s.config.limits.max_order_contracts / leg.ratio)
      return {Reason::MAX_ORDER_CONTRACTS, "A leg's contract count exceeds the order limit",
              static_cast<double>(r.quantity) * static_cast<double>(leg.ratio), static_cast<double>(s.config.limits.max_order_contracts), leg.symbol};
    const auto leg_tick = tick_size(c->second.root, Money{});
    tick = tick == Money{} ? leg_tick : std::min(tick, leg_tick);
  }
  if (r.limit_price && r.limit_price->micros() % tick.micros() != 0)
    return failure(Reason::INVALID_TICK, "Net price is not a multiple of the legs' smallest tick");
  const auto trigger_ok = [](const std::optional<Trigger>& t) {
    return !t || ((t->source == TriggerSource::Combo || t->source == TriggerSource::Underlying) &&
        (t->source != TriggerSource::Underlying || t->level > Money{}) &&
        (t->direction == TriggerDirection::AtOrAbove || t->direction == TriggerDirection::AtOrBelow));
  };
  const auto exit_ok = [&](const std::optional<ExitSpec>& e) {
    return !e || (e->trigger.has_value() != e->limit_price.has_value() && trigger_ok(e->trigger) &&
        (!e->limit_price || e->limit_price->micros() % tick.micros() == 0));
  };
  if (!trigger_ok(r.trigger) || (r.bracket && ((!r.bracket->stop_loss && !r.bracket->take_profit) ||
      !exit_ok(r.bracket->stop_loss) || !exit_ok(r.bracket->take_profit))))
    return failure(Reason::INVALID_ORDER, "Combo exits need a signed net limit on the leg tick or a combo/underlying trigger");
  if ((r.trigger || r.exits_only) && !closing_only(s, o))
    return failure(Reason::INVALID_ORDER, "Conditional combos and held exits must close held legs in ratio without exceeding them");
  if (r.exits_only) {
    if (!r.bracket || (r.type == OrderType::Limit && r.tif != TimeInForce::Gtc))
      return failure(Reason::INVALID_ORDER, "Held exits require a bracket and GTC for a limit exit");
    const auto& primary = r.bracket->take_profit ? *r.bracket->take_profit : *r.bracket->stop_loss;
    if (r.trigger != primary.trigger || r.limit_price != primary.limit_price)
      return failure(Reason::INVALID_ORDER, "Held exit terms must match the take-profit, or the stop when there is no target");
    return system_check(s, o);
  }
  if (r.bracket && std::any_of(r.legs.begin(), r.legs.end(), [&](const Leg& leg) {
        const auto q = held(s, leg.symbol);
        return q != 0 && (q > 0) != (leg.side == Side::Buy);
      }))
    return failure(Reason::INVALID_ORDER, "Entry brackets require opening legs; attach exits to held closing legs instead");
  if (rules.buy_only && !closing_only(s, o))
    return failure(Reason::BUY_ONLY, "This plan is buy-only; multi-leg orders may only close held positions");
  {
    std::vector<std::pair<std::string, Quantity>> legs;
    for (const auto& leg : r.legs) {
      const auto q = o.remaining() * leg.ratio;
      legs.emplace_back(leg.symbol, leg.side == Side::Buy ? q : -q);
    }
    if (auto d = defined_risk_check(s, legs); !d.ok()) return d;
  }
  for (const auto& leg : r.legs) {
    if (rules.expiry_cutoff > 0 && s.time >= s.contracts.at(leg.symbol).last_trade_time() - rules.expiry_cutoff && !closing_only(s, o))
      return failure(Reason::EXPIRY_CUTOFF, "A leg is inside the pre-expiry cutoff; only closing orders are accepted");
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
  const auto snapshot = measure(s);
  if (!snapshot.valuation_complete) return failure(Reason::STALE_QUOTE, "All held positions need fresh marks before trading");
  if (!closing_only(s, o))
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
  if (const auto d = account_check(s, closing_only(s, o)); !d.ok()) return d;
  try { (void)clean_annotation(o.request.note, o.request.tags); }
  catch (const TradingError& e) { return failure(e.code(), e.what()); }
  if (multi_leg(o.request)) return combo_check(s, o, at_fill);
  const auto& rules = s.config.rules;
  const auto& request = o.request;
  const auto c = s.contracts.find(request.symbol);
  if (c == s.contracts.end()) return failure(Reason::UNKNOWN_CONTRACT, "Order must reference a registered canonical OSI definition");
  if (s.time >= c->second.expiry_time()) return failure(Reason::EXPIRED, "Contract has expired");
  if (const auto d = session_check(c->second, s.time, request); !d.ok()) return d;
  if (request.client_order_id.empty() || request.quantity <= 0 ||
      (request.side != Side::Buy && request.side != Side::Sell) ||
      (request.type != OrderType::Market && request.type != OrderType::Limit) ||
      (request.tif != TimeInForce::Day && request.tif != TimeInForce::Ioc && request.tif != TimeInForce::Gtc) ||
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
  if (request.exits_only || (request.trigger && request.trigger->source == TriggerSource::Combo) ||
      (bracket && ((bracket->stop_loss && bracket->stop_loss->trigger && bracket->stop_loss->trigger->source == TriggerSource::Combo) ||
                   (bracket->take_profit && bracket->take_profit->trigger && bracket->take_profit->trigger->source == TriggerSource::Combo))) ||
      !positive(request.trigger) ||
      (bracket && (!exit_ok(bracket->stop_loss) || !exit_ok(bracket->take_profit) || (!bracket->stop_loss && !bracket->take_profit))))
    return failure(Reason::INVALID_ORDER, "Trigger levels and exit prices must be positive; each exit takes either a trigger or a limit price");
  if (bracket && bracket->take_profit && bracket->take_profit->limit_price &&
      bracket->take_profit->limit_price->micros() % tick_size(c->second.root, *bracket->take_profit->limit_price).micros() != 0)
    return failure(Reason::INVALID_TICK, "Take-profit price is not a positive multiple of the product tier tick");
  if (rules.buy_only && request.side == Side::Sell && !closing_only(s, o))
    return failure(Reason::BUY_ONLY, "This plan is buy-only: sells may only close contracts you already hold");
  if (auto d = defined_risk_check(s, {{request.symbol, request.side == Side::Buy ? o.remaining() : -o.remaining()}}); !d.ok())
    return d;
  if (rules.expiry_cutoff > 0 && s.time >= c->second.last_trade_time() - rules.expiry_cutoff && !closing_only(s, o))
    return failure(Reason::EXPIRY_CUTOFF, "Contract is inside the pre-expiry cutoff; only closing orders are accepted");
  if (const auto d = quote_check(s, request.symbol); !d.ok()) return d;
  const auto& quote = s.books.at(request.symbol).quote;
  const auto price = !at_fill && request.limit_price ? *request.limit_price
      : execution_price(s, request.symbol, request.side, request.limit_price);
  if (const auto d = price_check(s, quote, price); !d.ok()) return d;
  const auto snapshot = measure(s);
  if (!snapshot.valuation_complete) return failure(Reason::STALE_QUOTE, "All held positions need fresh marks before trading");
  if (!closing_only(s, o))
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
  if (multi_leg(o.request)) {
    if (!closing_only(s, o, false)) return failure(Reason::POSITION_CLOSED, "Exit legs no longer fit the held positions");
    std::vector<std::pair<std::string, Quantity>> legs;
    for (const auto& leg : o.request.legs) {
      const auto& c = s.contracts.at(leg.symbol);
      if (s.time >= c.last_trade_time()) return failure(Reason::EXPIRED, "An exit leg reached its last trade");
      if (!regular(c, s.time)) return failure(Reason::SESSION_CLOSED, "Combo exits wait for the regular session");
      if (auto d = quote_check(s, leg.symbol); !d.ok()) return d;
      legs.emplace_back(leg.symbol, signed_contracts(leg, o.remaining()));
    }
    return defined_risk_check(s, legs);
  }
  const auto c = s.contracts.find(o.request.symbol);
  if (c == s.contracts.end()) return failure(Reason::UNKNOWN_CONTRACT, "Order must reference a registered canonical OSI definition");
  if (s.time >= c->second.expiry_time()) return failure(Reason::EXPIRED, "Contract has expired");
  if (!regular(c->second, s.time))
    return failure(Reason::SESSION_CLOSED, "Closing orders the account places itself and bracket exits trade in the regular session only");
  // A bracket exit keeps a defined-risk plan's shorts covered; the account's own closing orders need not.
  if (!o.system && o.request.side == Side::Sell)
    if (auto d = defined_risk_check(s, {{o.request.symbol, -o.remaining()}},
                                    "Selling this long would leave a short option uncovered; close the short first");
        !d.ok())
      return d;
  return quote_check(s, o.request.symbol);
}
bool marketable(const Order& o, const QuoteObservation& q) {
  if (o.request.type == OrderType::Market) return true;
  return o.request.side == Side::Buy ? *q.ask <= *o.request.limit_price : *q.bid >= *o.request.limit_price;
}
void on_fill(State& s, OrderId id, Events& events);
void annotate_opening(State& s, const OrderRequest& r, const std::string& symbol, Quantity contracts, Events& events) {
  const auto q = held(s, symbol);
  if ((q != 0 && ((q > 0) == (contracts > 0) || magnitude(contracts) <= magnitude(q))) ||
      (r.note.empty() && r.tags.empty())) return;
  auto a = clean_annotation(r.note, r.tags);
  store_annotation(s, std::to_string(s.fills.size() + 1), std::move(a), events);
}
void observe_equity(State& s, Events& events);
/// Stale marks, an invalid quote or a missing valuation hold a fill back until a
/// later batch brings the data; they say nothing about the order itself.
bool data_gap(Reason code) {
  return code == Reason::STALE_QUOTE || code == Reason::INVALID_QUOTE || code == Reason::MISSING_VALUATION;
}
void match_one(State& s, OrderId id, Events& events, std::optional<OrderId> incoming) {
  // Read here; every write goes through mut() below, after which `o` is not read.
  const auto& o = s.orders.at(static_cast<std::size_t>(id - 1));
  if (!o.open() || o.status == OrderStatus::Armed || !latency_ready(s, o)) return;
  if (incoming != id && s.books.at(o.request.symbol).quote.time < o.accepted_at) return;
  // Good-until-expiry exits and triggered orders outlive a session; outside
  // the regular session they wait for the next one.
  if (persistent(o) && !regular(s.contracts.at(o.request.symbol), s.time)) return;
  if (!quote_check(s, o.request.symbol).ok() || !marketable(o, s.books.at(o.request.symbol).quote)) return;
  if (s.config.rules.impact_ticks > 0 && o.request.limit_price) {
    const auto candidate = execution_price(s, o.request.symbol, o.request.side);
    if (o.request.side == Side::Buy ? candidate > *o.request.limit_price : candidate < *o.request.limit_price) return;
  }
  // System orders and bracket exits only ever reduce a position (exits are kept
  // within it), so they skip the price band and loss projection when executing.
  const bool reducing = o.system || o.role != OrderRole::Normal;
  auto decision = reducing ? system_check(s, o) : order_check(s, o, true);
  if (data_gap(decision.code)) return;
  if (!decision.ok()) {
    decision.message = std::string(to_string(decision.code)) + ": " + decision.message;
    decision.code = Reason::RISK_CHANGED;
    cancel_order(s.orders.mut(id - 1), decision, events);
    return;
  }
  const auto symbol = o.request.symbol;
  const auto side = o.request.side;
  const auto& book = s.books.at(symbol);
  if (!marketable(o, book.quote)) return;
  const Money price = execution_price(s, symbol, side, o.request.limit_price);
  const auto remaining_depth = side == Side::Buy ? book.ask_left : book.bid_left;
  const auto size = side == Side::Buy ? book.quote.ask_size : book.quote.bid_size;
  const auto budget = s.config.rules.impact_ticks > 0 ? size - depth_used(size, remaining_depth) % size : remaining_depth;
  const auto position = held(s, o.request.symbol);
  const auto capacity = o.request.side == Side::Sell ? std::max<Quantity>(position, 0) : std::max<Quantity>(-position, 0);
  const Quantity quantity = reducing ? std::min({o.remaining(), budget, capacity}) : std::min(o.remaining(), budget);
  if (quantity <= 0) return;
  decision = reducing ? Decision{} : price_check(s, book.quote, price);
  const Money fee = s.config.fee_per_contract * quantity;
  if (decision.ok() && !reducing) {
    // Check the proposed accounting before committing any liquidity or fill.
    const Quantity signed_quantity = side == Side::Buy ? quantity : -quantity;
    State projected = s;
    projected.ledger.fill(s.contracts.at(symbol), signed_quantity, price, fee);
    projected.orders.mut(static_cast<std::size_t>(id - 1)).filled_quantity += quantity;
    if (!closing_only(s, o)) decision = loss_check(projected, measure(projected));
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
    cancel_order(s.orders.mut(id - 1), decision, events);
    return;
  }
  const auto context = fill_context(s, symbol, measure(s));
  annotate_opening(s, o.request, symbol, side == Side::Buy ? quantity : -quantity, events);
  const auto realised_before = s.ledger.account().realised;
  const bool opening = quantity > capacity;
  const auto quote = book.quote;
  fill_position(s, symbol, side == Side::Buy ? quantity : -quantity, price, fee);
  auto& left = s.books[symbol];
  consume_depth(side == Side::Buy ? left.ask_left : left.bid_left, quantity);
  auto& order = s.orders.mut(id - 1);
  order.filled_quantity += quantity;
  order.filled_notional = order.filled_notional + price * quantity;
  order.status = order.remaining() == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
  Fill fill{static_cast<std::uint64_t>(s.fills.size() + 1), id, symbol, side,
            quantity, price, fee, quote.observation, quote.time, s.time, context, order.actor};
  s.fills.push_back(fill);
  event(events, "fill", fill);
  on_fill(s, id, events);
  guardrail_fill(s, id, opening, realised_before, events);
  observe_equity(s, events);
}
/// A multi-leg order fills all its legs together, in ratio, at each leg's slipped
/// far side when the net debit is at or below its limit; units are bounded by every
/// leg's remaining displayed size. The whole projected fill is rechecked first.
void match_combo(State& s, OrderId id, Events& events, std::optional<OrderId> incoming) {
  // Read here; every write goes through mut() below, after which `o` is not read.
  const auto& o = s.orders.at(static_cast<std::size_t>(id - 1));
  if (!o.open() || o.status == OrderStatus::Armed || !latency_ready(s, o)) return;
  for (const auto& leg : o.request.legs) {
    if (persistent(o) && !regular(s.contracts.at(leg.symbol), s.time)) return;
    if (!quote_check(s, leg.symbol).ok()) return;
    if (incoming != id && s.books.at(leg.symbol).quote.time < o.accepted_at) return;
  }
  const auto net = executable_net(s, o.request);
  if (!net || (o.request.limit_price && *net > *o.request.limit_price)) return;
  const bool exit = o.role != OrderRole::Normal;
  auto decision = exit ? system_check(s, o) : order_check(s, o, true);
  if (data_gap(decision.code)) return;
  Quantity units = s.config.rules.impact_ticks > 0 ? 1 : o.remaining();
  if (s.config.rules.impact_ticks == 0) for (const auto& leg : o.request.legs) {
    const auto& book = s.books.at(leg.symbol);
    units = std::min(units, (leg.side == Side::Buy ? book.ask_left : book.bid_left) / leg.ratio);
  }
  if (decision.ok() && units <= 0) return;
  const auto slices = combo_slices(s, o.request, units);
  if (decision.ok() && !exit) {
    State projected = s;
    for (const auto& slice : slices) {
      const auto contracts = slice.side == Side::Buy ? slice.quantity : -slice.quantity;
      projected.ledger.fill(s.contracts.at(slice.symbol), contracts, slice.price, s.config.fee_per_contract * slice.quantity);
    }
    projected.orders.mut(static_cast<std::size_t>(id - 1)).filled_quantity += units;
    if (!closing_only(s, o)) decision = loss_check(projected, measure(projected));
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
    cancel_order(s.orders.mut(id - 1), decision, events);
    return;
  }
  const auto request = o.request;
  const auto actor = o.actor;
  const auto before = measure(s);
  const auto realised_before = s.ledger.account().realised;
  const bool opening = std::any_of(request.legs.begin(), request.legs.end(), [&](const Leg& leg) {
    const auto position = held(s, leg.symbol);
    return units * leg.ratio > (leg.side == Side::Buy ? std::max<Quantity>(0, -position) : std::max<Quantity>(0, position));
  });
  for (const auto& slice : slices) {
    const auto context = fill_context(s, slice.symbol, before);
    const auto quote = s.books.at(slice.symbol).quote;
    const auto contracts = slice.side == Side::Buy ? slice.quantity : -slice.quantity;
    const auto size = magnitude(contracts);
    const Money price = slice.price;
    const Money fee = s.config.fee_per_contract * size;
    annotate_opening(s, request, slice.symbol, contracts, events);
    fill_position(s, slice.symbol, contracts, price, fee);
    auto& book = s.books[slice.symbol];
    consume_depth(slice.side == Side::Buy ? book.ask_left : book.bid_left, size);
    Fill fill{static_cast<std::uint64_t>(s.fills.size() + 1), id, slice.symbol, slice.side,
              size, price, fee, quote.observation, quote.time, s.time, context, actor};
    s.fills.push_back(fill);
    event(events, "fill", fill);
  }
  auto& order = s.orders.mut(id - 1);
  order.filled_quantity += units;
  order.filled_notional = order.filled_notional + *net * units;
  order.status = order.remaining() == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
  on_fill(s, id, events);
  guardrail_fill(s, id, opening, realised_before, events);
  observe_equity(s, events);
}
/// Sweep successive simulated tiers, retaining the original one-fill path when
/// impact is off. A delayed IOC gets its one attempt on an eligible quote.
void match_order(State& s, OrderId id, Events& events, std::optional<OrderId> incoming) {
  const auto& initial = s.orders[id - 1];
  if (!initial.open() || initial.status == OrderStatus::Armed || !latency_ready(s, initial)) return;
  const bool combo = multi_leg(initial.request);
  for (;;) {
    const auto before = s.orders[id - 1].filled_quantity;
    if (combo) match_combo(s, id, events, incoming); else match_one(s, id, events, incoming);
    const auto& current = s.orders[id - 1];
    if (s.config.rules.impact_ticks == 0 || !current.open() || current.filled_quantity == before) break;
  }
  const auto& stored = s.orders[id - 1];
  if (s.config.rules.fill_latency_ms > 0 && stored.open() && stored.request.tif == TimeInForce::Ioc)
    cancel_order(s.orders.mut(id - 1), failure(Reason::IOC_REMAINDER, "IOC exhausted eligible liquidity"), events);
}
void match_symbols(State& s, const std::set<std::string>& symbols, Events& events,
                   std::optional<OrderId> incoming = {}) {
  for (const auto& symbol : symbols) {
    for (const auto side : {Side::Buy, Side::Sell}) {
      std::vector<OrderId> priority;
      for (const auto id : open_ids(s)) {
        const auto& o = s.orders[id - 1];
        if (o.status != OrderStatus::Armed && o.request.symbol == symbol && o.request.side == side) priority.push_back(id);
      }
      std::stable_sort(priority.begin(), priority.end(), [&](OrderId a, OrderId b) {
        const auto& x = s.orders.at(static_cast<std::size_t>(a - 1));
        const auto& y = s.orders.at(static_cast<std::size_t>(b - 1));
        if (x.request.type != y.request.type) return x.request.type == OrderType::Market;
        if (x.request.limit_price == y.request.limit_price) return a < b;
        return side == Side::Buy ? x.request.limit_price > y.request.limit_price : x.request.limit_price < y.request.limit_price;
      });
      for (auto id : priority) match_order(s, id, events, incoming);
    }
  }
  // Multi-leg orders then take the displayed liquidity left, in acceptance order.
  std::vector<OrderId> combos;
  for (const auto id : open_ids(s)) {
    const auto& o = s.orders[id - 1];
    if (multi_leg(o.request) &&
        std::any_of(o.request.legs.begin(), o.request.legs.end(), [&](const Leg& leg) { return symbols.contains(leg.symbol); }))
      combos.push_back(id);
  }
  for (const auto id : combos) match_order(s, id, events, incoming);
}
/// Keep bracket exits within the position they protect: shrink them when it
/// shrinks and cancel them once it is closed, so an exit can never open one.
void sync_exits(State& s, const std::string& symbol, Events& events) {
  for (const auto id : open_ids(s)) {
    const auto& o = s.orders[id - 1];
    if (!o.open() || o.role == OrderRole::Normal || !touches(o.request, symbol)) continue;
    auto capacity = std::numeric_limits<Quantity>::max();
    const auto cap = [&](const std::string& leg_symbol, Side side, Quantity ratio) {
      const auto q = held(s, leg_symbol);
      capacity = std::min(capacity, (side == Side::Sell ? std::max<Quantity>(q, 0) : std::max<Quantity>(-q, 0)) / ratio);
    };
    if (multi_leg(o.request)) for (const auto& leg : o.request.legs) cap(leg.symbol, leg.side, leg.ratio);
    else cap(o.request.symbol, o.request.side, 1);
    if (capacity == 0) {
      cancel_order(s.orders.mut(id - 1), failure(Reason::POSITION_CLOSED, "The position this exit protected is closed"), events);
    } else if (o.remaining() > capacity) {
      auto& exit = s.orders.mut(id - 1);
      exit.request.quantity = exit.filled_quantity + capacity;
      event(events, "order_resized", exit);
    }
  }
}
/// Create a bracket's exits on the entry's first fill and grow them with later
/// fills. A stop is armed until reached, a limit take-profit rests; both are good
/// until expiry, sized to the filled quantity, and cancel each other on a fill.
void attach_exits(State& s, OrderId entry_id, Events& events) {
  const auto entry = s.orders.at(static_cast<std::size_t>(entry_id - 1));  // pushes below invalidate references
  const auto& bracket = *entry.request.bracket;
  const auto expiry = multi_leg(entry.request) ? order_expiry(s, entry.request) : s.contracts.at(entry.request.symbol).expiry_time();
  auto make = [&](const ExitSpec& spec, OrderRole role) {
    Order exit;
    exit.actor = entry.actor;
    exit.id = static_cast<OrderId>(s.orders.size() + 1);
    exit.request = {entry.request.client_order_id + (role == OrderRole::StopLoss ? ":stop" : ":target"),
                    entry.request.symbol, entry.request.side == Side::Buy ? Side::Sell : Side::Buy,
                    spec.trigger ? OrderType::Market : OrderType::Limit, spec.trigger ? TimeInForce::Ioc : TimeInForce::Day,
                    entry.filled_quantity, spec.limit_price, spec.trigger, {}, {}};
    if (multi_leg(entry.request)) {
      exit.request.side = Side::Buy;
      exit.request.legs = entry.request.legs;
      for (auto& leg : exit.request.legs) leg.side = leg.side == Side::Buy ? Side::Sell : Side::Buy;
      if (exit.request.type == OrderType::Limit) exit.request.tif = TimeInForce::Gtc;
    }
    exit.role = role;
    exit.parent = entry.id;
    exit.status = spec.trigger ? OrderStatus::Armed : OrderStatus::Working;
    exit.accepted_at = s.time;
    exit.day_end = expiry;
    add_order(s, exit);
    event(events, "order_accepted", exit);
    return exit.id;
  };
  auto grow = [&](OrderId id) {
    const auto& exit = s.orders.at(static_cast<std::size_t>(id - 1));
    if (exit.open() && exit.filled_quantity == 0 && exit.request.quantity < entry.filled_quantity) {
      auto& resized = s.orders.mut(id - 1);
      resized.request.quantity = entry.filled_quantity;
      event(events, "order_resized", resized);
    }
  };
  auto stop = entry.stop_loss, target = entry.take_profit;
  const bool created = (bracket.stop_loss && stop == 0) || (bracket.take_profit && target == 0);
  if (bracket.stop_loss) { if (stop == 0) stop = make(*bracket.stop_loss, OrderRole::StopLoss); else grow(stop); }
  if (bracket.take_profit) { if (target == 0) target = make(*bracket.take_profit, OrderRole::TakeProfit); else grow(target); }
  auto& stored = s.orders.mut(entry_id - 1);
  stored.stop_loss = stop;
  stored.take_profit = target;
  if (created && stop != 0 && target != 0) {
    s.orders.mut(stop - 1).oco = target;
    s.orders.mut(target - 1).oco = stop;
  }
}
void on_fill(State& s, OrderId id, Events& events) {
  detail::update_reviews(s);
  const auto oco = s.orders.at(static_cast<std::size_t>(id - 1)).oco;
  if (oco != 0 && s.orders.at(static_cast<std::size_t>(oco - 1)).open())
    cancel_order(s.orders.mut(oco - 1), failure(Reason::OCO_FILLED, "The other exit of this bracket filled"), events);
  const auto order = s.orders.at(static_cast<std::size_t>(id - 1));
  if (order.role != OrderRole::Normal && order.parent != 0 && s.orders.at(static_cast<std::size_t>(order.parent - 1)).open())
    cancel_order(s.orders.mut(order.parent - 1), failure(Reason::OCO_FILLED, "An exit filled; the remaining entry is cancelled"), events);
  if (order.request.bracket && !order.request.exits_only) attach_exits(s, id, events);
  for (const auto& symbol : order_symbols(s.orders.at(static_cast<std::size_t>(id - 1)).request)) sync_exits(s, symbol, events);
}
/// Option triggers read the order's executable side from a fresh book; underlying
/// triggers read spot from a fresh valuation. Missing data never triggers.
bool reached(const State& s, const Order& o) {
  const auto& t = *o.request.trigger;
  std::optional<Money> value;
  if (t.source == TriggerSource::Underlying) {
    const auto it = s.valuations.find(order_symbols(o.request).front());
    if (it != s.valuations.end() && valid_valuation(it->second) && it->second.time <= s.time &&
        s.time - it->second.time <= s.config.limits.max_valuation_age) {
      try { value = Money::from_double(it->second.spot); } catch (const TradingError&) {}
    }
  } else if (t.source == TriggerSource::Combo && multi_leg(o.request)) {
    Money net;
    for (const auto& leg : o.request.legs) {
      if (!quote_check(s, leg.symbol).ok()) return false;
      const auto& q = s.books.at(leg.symbol).quote;
      net = net + (leg.side == Side::Buy ? *q.ask : -*q.bid) * leg.ratio;
    }
    value = net;
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
    const auto& o = s.orders.at(static_cast<std::size_t>(id - 1));
    auto d = o.role != OrderRole::Normal ? system_check(s, o) : order_check(s, o);
    if (data_gap(d.code) || d.code == Reason::SESSION_CLOSED || d.code == Reason::LIMIT_ONLY) return;
    if (!d.ok()) {
      d.message = std::string(to_string(d.code)) + ": " + d.message;
      d.code = Reason::RISK_CHANGED;
      cancel_order(s.orders.mut(id - 1), d, events);
      return;
    }
    auto& order = s.orders.mut(id - 1);
    order.status = OrderStatus::Working;
    order.triggered_at = s.time;
    event(events, "order_triggered", order);
  }
  const auto symbols = order_symbols(s.orders.at(static_cast<std::size_t>(id - 1)).request);
  match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
  const auto& stored = s.orders.at(static_cast<std::size_t>(id - 1));
  if (s.config.rules.fill_latency_ms == 0 && stored.open() && stored.request.tif == TimeInForce::Ioc)
    cancel_order(s.orders.mut(id - 1), failure(Reason::IOC_REMAINDER, "IOC exhausted available displayed liquidity"), events);
}
/// Armed orders activate only in their contract's regular session.
void check_triggers(State& s, Events& events) {
  const auto visit = [&](OrderId id) {
    const auto& o = s.orders[id - 1];
    if (o.status != OrderStatus::Armed || !regular(s.contracts.at(order_symbols(o.request).front()), s.time) || !reached(s, o))
      return;
    activate(s, id, events);
  };
  // The open orders in ID order, then any placed while activating them (a
  // bracket's exits), in order, as one pass over every order would find them.
  const auto placed = s.orders.size();
  for (const auto id : open_ids(s)) if (id <= placed) visit(id);
  for (auto i = placed; i < s.orders.size(); ++i) visit(static_cast<OrderId>(i + 1));
}
/// Submit a reducer-owned market IOC that closes one position against the
/// current fresh book. Without executable liquidity nothing is recorded, so a
/// rule keeps retrying on later transactions instead of accumulating orders.
void flatten(State& s, const std::string& symbol, std::string_view why, Events& events) {
  const auto q = held(s, symbol);
  const auto contract = s.contracts.find(symbol);
  if (q == 0 || contract == s.contracts.end() || s.time >= contract->second.expiry_time() ||
      !regular(contract->second, s.time) || !quote_check(s, symbol).ok())
    return;
  const auto side = q > 0 ? Side::Sell : Side::Buy;
  const auto& book = s.books.at(symbol);
  if (s.config.rules.impact_ticks == 0 && (side == Side::Sell ? book.bid_left : book.ask_left) <= 0) return;
  for (const auto id : open_ids(s))
    if (const auto& pending = s.orders[id - 1]; pending.open() && pending.system && pending.request.symbol == symbol) return;
  Order order;
  order.id = static_cast<OrderId>(s.orders.size() + 1);
  order.request = {"system:" + std::string(why) + ":" + std::to_string(order.id), symbol, side,
                   OrderType::Market, TimeInForce::Ioc, magnitude(q), {}, {}, {}, {}};
  order.accepted_at = s.time;
  order.day_end = s.config.rules.fill_latency_ms > 0 ? session_end(contract->second, s.time) : s.time;
  order.system = true;
  order.actor = "system";
  add_order(s, order);
  event(events, "order_accepted", order);
  match_symbols(s, {symbol}, events, order.id);
  if (s.config.rules.fill_latency_ms == 0 && s.orders.at(static_cast<std::size_t>(order.id - 1)).open())
    cancel_order(s.orders.mut(order.id - 1), failure(Reason::IOC_REMAINDER, "IOC exhausted available displayed liquidity"), events);
}
/// Close shares at the underlying's fresh price in the regular session; without
/// one they stay, and a rule retries on later transactions.
void close_shares(State& s, const std::string& symbol, Quantity shares, StockSource source, Events& events) {
  const auto price = stock_price(s, symbol);
  if (shares == 0 || !price || !md::market_session(s.time).open) return;
  trade_shares(s, symbol, -shares, *price, source);
  event(events, "stock_trade", Json{{"symbol", symbol}, {"shares", -shares}, {"price", *price}});
}
void decide(State& s, EvaluationStatus status, Money equity, std::string message, Events& events) {
  auto& e = s.evaluation;
  e.status = status;
  e.decided_at = s.time;
  e.decided_equity = equity;
  e.decision = message;
  event(events, status == EvaluationStatus::Passed ? "evaluation_passed" : "evaluation_failed",
        Json{{"attempt", e.attempt}, {"equity", equity}, {"peak", e.peak}, {"floor", e.floor}, {"message", message}});
  for (const auto id : open_ids(s))
    if (!s.orders[id - 1].system) cancel_order(s.orders.mut(id - 1), failure(Reason::EVALUATION_CLOSED, message), events);
}
/// Runs after every command: tracks the day's closing equity, ratchets an
/// intraday peak, decides pass/fail on fully marked equity (touching the floor
/// fails), then liquidates a decided attempt and auto-closes expiring positions.
void observe_equity(State& s, Events& events) {
  const auto& rules = s.config.rules;
  auto& e = s.evaluation;
  // A journal created before any market data starts at time zero; the attempt
  // begins at the first real market time instead.
  if (e.started == 0 && s.time > 0) e.started = s.time;
  // Likewise the first payout cycle; journals from before payouts start it here too.
  if (e.cycle_started == 0) e.cycle_started = e.started;
  if (const auto equity = marked_equity(measure(s))) {
    if (md::trading_date(s.time) == e.day) {
      e.day_close_equity = *equity;
      if (!e.day_low_equity || *equity < *e.day_low_equity) { e.day_low_equity = *equity; e.day_low_at = s.time; }
      if (!e.day_high_equity || *equity > *e.day_high_equity) { e.day_high_equity = *equity; e.day_high_at = s.time; }
    }
    const auto outcome = evaluate_equity(e, rules, *equity);
    if (outcome != e.status) {
      const auto target = e.starting_balance + rules.profit_target;
      if (outcome == EvaluationStatus::Failed) {
        decide(s, outcome, *equity, "Equity " + dollars(*equity) + " reached the drawdown floor " +
               dollars(e.floor) + " (peak " + dollars(e.peak) + ", max drawdown " + dollars(rules.max_drawdown) + ")", events);
      } else {
        decide(s, outcome, *equity, "Equity " + dollars(*equity) + " reached the profit target " + dollars(target), events);
      }
    }
    if (rules.max_drawdown > Money{} && (!e.closest_floor || *equity - e.floor < *e.closest_floor)) {
      e.closest_floor = *equity - e.floor;
      e.closest_floor_at = s.time;
    }
    const auto soft_floor = soft_floor_of(s);
    const auto& g = s.config.guardrails;
    if (soft_floor && *equity <= *soft_floor) latch_guardrail(s, Reason::SOFT_FLOOR, events);
    if (g.profit_lock > Money{} && *equity - s.start_equity >= g.profit_lock) latch_guardrail(s, Reason::PROFIT_LOCK, events);
    if (g.max_opening_trades > 0 && s.guardrails.opening_trades >= g.max_opening_trades)
      latch_guardrail(s, Reason::TRADE_LIMIT, events);
  }
}
void monitor_rules(State& s, Events& events) {
  observe_equity(s, events);
  const auto& rules = s.config.rules;
  const auto& e = s.evaluation;
  const bool soft = std::find(s.guardrails.latched.begin(), s.guardrails.latched.end(), Reason::SOFT_FLOOR) != s.guardrails.latched.end();
  if (soft || (rules.evaluation() && e.status != EvaluationStatus::Active)) {
    std::vector<std::pair<std::string, Quantity>> stocks;
    for (const auto& [symbol, stock] : s.ledger.stocks()) stocks.emplace_back(symbol, stock.shares);
    for (const auto& [symbol, shares] : stocks) close_shares(s, symbol, shares, StockSource::Rule, events);
  }
  std::vector<std::string> symbols;
  for (const auto& [symbol, position] : s.ledger.positions()) symbols.push_back(symbol);
  for (const auto& symbol : symbols) {
    if (soft || (rules.evaluation() && e.status != EvaluationStatus::Active)) {
      flatten(s, symbol, soft ? "soft_floor" : e.status == EvaluationStatus::Passed ? "target" : "drawdown", events);
      continue;
    }
    // The cutoff counts back from the last trade: 15:55 for SPXW, 16:10 for SPY,
    // and the afternoon before expiry for AM-settled series.
    const auto& contract = s.contracts.at(symbol);
    if (rules.expiry_cutoff > 0 && s.time >= contract.last_trade_time() - rules.expiry_cutoff &&
        s.time < contract.last_trade_time()) {
      for (const auto id : open_ids(s))
        if (const auto& o = s.orders[id - 1]; !o.system && touches(o.request, symbol))
          cancel_order(s.orders.mut(id - 1), failure(Reason::EXPIRY_CUTOFF, "Pre-expiry cutoff: the position is being closed"), events);
      flatten(s, symbol, "expiry", events);
    }
  }
}
/// Both submit and preview check the candidate while its reservation is present.
Decision acceptance_check(const State& s, const Order& candidate, const Decision& rejection) {
  // Another order used this ID first.
  if (const auto first = s.clients.find(candidate.request.client_order_id);
      first != s.clients.end() && first->second != candidate.id)
    return failure(Reason::DUPLICATE_CLIENT_ID, "client_order_id already used");
  if (!rejection.ok()) return rejection;
  auto decision = order_check(s, candidate);
  return decision.ok() ? open_orders_risk_check(s, candidate) : decision;
}
std::map<std::string, double> fresh_stock_prices(const State& s) {
  std::map<std::string, double> prices;
  for (const auto& [symbol, stock] : s.ledger.stocks())
    if (const auto price = stock_price(s, symbol)) prices[symbol] = price->dollars();
  return prices;
}
BreachRisk breach_of(const State& s, const std::map<std::string, double>& close_variances) {
  const auto snapshot = measure(s);
  auto result = breach_risk(s.ledger, s.valuations, snapshot.equity,
      s.config.rules.max_drawdown > Money{} ? std::optional(s.evaluation.floor) : std::nullopt,
      snapshot.soft_floor, s.time, s.config.limits.max_valuation_age, fresh_stock_prices(s), close_variances);
  result.complete &= snapshot.valuation_complete;
  if (!snapshot.valuation_complete) {
    result.room.reset(); result.soft_room.reset();
    for (auto& underlying : result.underlyings) {
      underlying.complete = false;
      underlying.down.reset(); underlying.up.reset();
    }
  }
  return result;
}
/// A bounded same-expiry payoff is piecewise linear: its minimum is at zero or
/// a strike, unless its terminal call slope is negative (unbounded loss).
std::optional<Money> expiry_loss(const State& s, const OrderRequest& request, Money premium, Money fees) {
  auto legs = request.legs;
  if (legs.empty()) legs.push_back({request.symbol, request.side, 1});
  const auto& first = s.contracts.at(legs.front().symbol);
  Quantity call_slope = 0;
  std::vector<Money> knots{Money{}};
  for (const auto& leg : legs) {
    const auto& contract = s.contracts.at(leg.symbol);
    if (contract.expiry_time() != first.expiry_time() || contract.underlying != first.underlying) return {};
    if (contract.type == pricing::OptionType::Call) call_slope += leg.side == Side::Buy ? leg.ratio : -leg.ratio;
    knots.push_back(Money::from_double(contract.strike));
  }
  if (call_slope < 0) return {};
  std::optional<Money> minimum;
  for (const auto spot : knots) {
    Money payoff;
    for (const auto& leg : legs) {
      const auto& contract = s.contracts.at(leg.symbol);
      const auto strike = Money::from_double(contract.strike);
      const auto intrinsic = std::max(Money{}, contract.type == pricing::OptionType::Call ? spot - strike : strike - spot);
      payoff = payoff + (intrinsic * contract.multiplier) * signed_contracts(leg, request.quantity);
    }
    minimum = minimum ? std::min(*minimum, payoff) : payoff;
  }
  return std::max(Money{}, premium + fees - *minimum);
}
struct PreviewProjection {
  OrderPreview result;
  State projected;
};
PreviewProjection project_order(const State& before, OrderRequest request, const Decision& rejection) {
  PreviewProjection projection{{}, before};
  auto& result = projection.result;
  auto& after = projection.projected;
  const auto snapshot = snapshot_of(before);
  result.buying_power_before = snapshot.buying_power.available;
  Order candidate;
  candidate.id = static_cast<OrderId>(after.orders.size() + 1);
  candidate.request = request;
  candidate.accepted_at = before.time;
  add_order(after, candidate);
  result.decision = acceptance_check(after, after.orders.back(), rejection);
  auto legs = request.legs;
  if (legs.empty()) legs.push_back({request.symbol, request.side, 1});
  if (request.quantity <= 0 || legs.size() > kMaxLegs) return projection;
  for (const auto& leg : legs)
    if (leg.ratio <= 0 || leg.ratio > kMaxRatio || request.quantity > std::numeric_limits<Quantity>::max() / (100 * leg.ratio) ||
        !before.contracts.contains(leg.symbol) || !quote_check(before, leg.symbol).ok()) return projection;
  const auto power = buying_power(after, candidate.id);
  result.buying_power_required = power.focus_reservation;
  after.orders.mut_back().status = OrderStatus::Filled;
  Money premium, fees;
  for (const auto& leg : legs) {
    const auto quantity = signed_contracts(leg, request.quantity);
    const auto price = execution_price(before, leg.symbol, leg.side,
        legs.size() == 1 ? request.limit_price : std::nullopt);
    const auto fee = before.config.fee_per_contract * magnitude(quantity);
    premium = premium + (price * 100) * quantity;
    fees = fees + fee;
    after.ledger.fill(before.contracts.at(leg.symbol), quantity, price, fee);
  }
  // For a resting limit, project its limit debit/credit, not an impossible fill
  // at today's far sides. This also makes size-to-floor conservative at limits.
  if (request.limit_price) {
    const auto limit_premium = (*request.limit_price * 100) * request.quantity *
        (legs.size() == 1 && request.side == Side::Sell ? -1 : 1);
    auto account = after.ledger.account();
    account.cash = account.cash + premium - limit_premium;
    after.ledger = Ledger::restore(account, after.ledger.positions(), after.ledger.stocks());
    premium = limit_premium;
  }
  const auto projected = snapshot_of(after);
  result.buying_power_after = projected.buying_power.available;
  if (snapshot.risk.complete && projected.risk.complete) {
    const auto& a = snapshot.risk.aggregate.position;
    const auto& b = projected.risk.aggregate.position;
    result.exposure_change = Exposure{b.dollar_delta - a.dollar_delta, b.dollar_gamma_1pct - a.dollar_gamma_1pct,
                                      b.vega - a.vega, b.theta - a.theta};
  }
  if (!snapshot.valuation_complete || !projected.valuation_complete) return projection;
  result.max_loss = expiry_loss(before, request, premium, fees);
  if (result.max_loss) {
    result.max_loss_basis = "expiry_payoff";
  } else if (projected.scenarios.complete) {
    double worst = 0;
    for (const auto& cell : projected.scenarios.cells) worst = std::min(worst, cell.pnl);
    result.max_loss = std::max(Money{}, snapshot.equity - projected.equity - Money::from_double(worst));
    result.max_loss_basis = "scenario_grid";
  }
  if (result.max_loss) {
    result.equity_at_max_loss = snapshot.equity - *result.max_loss;
    if (before.config.rules.max_drawdown > Money{}) result.breaches_floor = *result.equity_at_max_loss <= before.evaluation.floor;
    if (snapshot.soft_floor) result.breaches_soft_floor = *result.equity_at_max_loss <= *snapshot.soft_floor;
  }
  return projection;
}

/// Accept or reject one new order; once accepted, arm it or match it at once.
CommandResult place(State& s, OrderRequest request, Timestamp time, const Decision& rejection, Events& events) {
  Order order;
  order.id = static_cast<OrderId>(s.orders.size() + 1);
  order.request = std::move(request);
  order.actor = s.actor;
  order.accepted_at = time;
  add_order(s, order);
  // Written until matching starts; nothing copies the orders before that.
  auto& stored = s.orders.mut_back();
  auto decision = acceptance_check(s, stored, rejection);
  if (!decision.ok()) {
    stored.status = OrderStatus::Rejected;
    stored.reason = decision;
    event(events, "order_rejected", stored);
    return CommandResult{decision, stored.id, 0};
  }
  const auto id = stored.id;
  const auto symbols = order_symbols(stored.request);
  const auto& contract = s.contracts.at(symbols.front());
  if (stored.request.exits_only) {
    const auto bracket = *stored.request.bracket;
    stored.role = bracket.take_profit ? OrderRole::TakeProfit : OrderRole::StopLoss;
    stored.day_end = order_expiry(s, stored.request);
    stored.status = stored.request.trigger ? OrderStatus::Armed : OrderStatus::Working;
    stored.take_profit = bracket.take_profit ? id : 0;
    stored.stop_loss = bracket.take_profit ? 0 : id;
    if (bracket.take_profit && bracket.stop_loss) {
      Order stop = stored;
      stop.id = static_cast<OrderId>(s.orders.size() + 1);
      stop.request.client_order_id += ":stop";
      stop.request.bracket.reset();
      stop.request.exits_only = false;
      stop.request.limit_price = bracket.stop_loss->limit_price;
      stop.request.trigger = bracket.stop_loss->trigger;
      stop.request.type = stop.request.trigger ? OrderType::Market : OrderType::Limit;
      stop.request.tif = stop.request.trigger ? TimeInForce::Ioc : TimeInForce::Gtc;
      stop.status = stop.request.trigger ? OrderStatus::Armed : OrderStatus::Working;
      stop.role = OrderRole::StopLoss;
      stop.oco = id;
      stop.stop_loss = 0;
      stop.take_profit = 0;
      stored.stop_loss = stop.id;
      stored.oco = stop.id;
      event(events, "order_accepted", stored);
      add_order(s, stop);
      event(events, "order_accepted", stop);
    } else event(events, "order_accepted", stored);
    match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
    check_triggers(s, events);
    return CommandResult{{}, id, 0};
  }
  if (stored.request.trigger) {
    // Armed until reached, and good until expiry; a level already reached
    // activates at once.
    stored.status = OrderStatus::Armed;
    stored.day_end = multi_leg(stored.request) ? order_expiry(s, stored.request) : contract.expiry_time();
    event(events, "order_accepted", stored);
    if (regular(contract, s.time) && reached(s, s.orders.at(static_cast<std::size_t>(id - 1))))
      activate(s, id, events);
    return CommandResult{{}, id, 0};
  }
  stored.day_end = stored.request.tif == TimeInForce::Gtc ? order_expiry(s, stored.request) : session_end(contract, time);
  event(events, "order_accepted", stored);
  // Existing better orders share any remaining budget even on command ingress.
  // Matching can append bracket exits, so re-read the order by ID afterwards.
  match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
  const auto& accepted = s.orders.at(static_cast<std::size_t>(id - 1));
  if (s.config.rules.fill_latency_ms == 0 && accepted.open() && accepted.request.tif == TimeInForce::Ioc)
    cancel_order(s.orders.mut(id - 1), failure(Reason::IOC_REMAINDER, "IOC exhausted available displayed liquidity"), events);
  return CommandResult{{}, id, 0};
}
std::string underlying_of(const State& s, const Order& o) {
  const auto c = s.contracts.find(order_symbols(o.request).front());
  return c == s.contracts.end() ? std::string{} : c->second.underlying;
}
/// Apply new terms to a resting order, or leave it untouched with the reason.
CommandResult change_order(State& s, OrderId id, const OrderChange& change, const Decision& rejection, Events& events) {
  if (id == 0 || id > s.orders.size()) return {failure(Reason::UNKNOWN_ORDER, "Unknown order ID"), {}, 0};
  // Written until matching or activation; nothing copies the orders before that.
  auto& order = s.orders.mut(static_cast<std::size_t>(id - 1));
  if (!order.open()) return {failure(Reason::ORDER_TERMINAL, "Order is already terminal"), id, 0};
  const auto& r = order.request;
  const bool exit = order.role != OrderRole::Normal;
  const bool resting = order.status == OrderStatus::Armed || (r.type == OrderType::Limit && r.tif != TimeInForce::Ioc);
  if (order.system || !resting)
    return {failure(Reason::INVALID_ORDER, "Only resting orders change: DAY/GTC limit orders, armed orders and bracket exits"), id, 0};
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
    const auto& root = s.contracts.at(order_symbols(order.request).front()).root;
    const auto& limit = order.request.limit_price;
    if ((order.request.trigger && order.request.trigger->source != TriggerSource::Combo && order.request.trigger->level <= Money{}) ||
        (!multi_leg(order.request) && limit && *limit <= Money{}))
      decision = failure(Reason::INVALID_ORDER, "Trigger levels and exit prices must be positive");
    else if (limit && limit->micros() % tick_size(root, multi_leg(order.request) ? Money{} : *limit).micros() != 0)
      decision = failure(Reason::INVALID_TICK, "Take-profit price is not a positive multiple of the product tier tick");
  } else {
    decision = order_check(s, order);
    if (decision.ok()) decision = open_orders_risk_check(s, order);
  }
  if (!decision.ok()) {
    order = before;
    return {decision, id, 0};
  }
  event(events, "order_modified", order);
  const auto symbols = order_symbols(order.request);
  if (order.status == OrderStatus::Armed) {
    if (regular(s.contracts.at(symbols.front()), s.time) && reached(s, order))
      activate(s, id, events);
  } else {
    match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
  }
  return {{}, id, 0};
}
/// Trims a note and its tags, and lowercases the tags: text without control
/// characters (a note may keep newlines and tabs, with a CRLF or lone CR line
/// break kept as a newline), a note of at most 2,000 bytes, at most eight distinct
/// tags of 1 to 32 bytes without commas.
Annotation clean_annotation(std::string note, const std::vector<std::string>& tags) {
  const auto invalid = [](std::string message) { throw TradingError(Reason::INVALID_NOTE, std::move(message)); };
  const auto newlines = [](std::string text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '\r') out.push_back(text[i]);
      else if (i + 1 == text.size() || text[i + 1] != '\n') out.push_back('\n');
    }
    return out;
  };
  const auto trim = [](std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string{};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
  };
  const auto text = [](std::string_view value, bool lines) {
    return std::none_of(value.begin(), value.end(), [&](unsigned char c) {
      return (c < 0x20 && !(lines && (c == '\n' || c == '\t'))) || c == 0x7f;
    });
  };
  Annotation a;
  a.note = trim(newlines(std::move(note)));
  if (a.note.size() > 2000) invalid("A note is at most 2,000 bytes of text");
  if (!text(a.note, true)) invalid("A note is text: no control characters other than newlines and tabs");
  for (const auto& tag : tags) {
    auto clean = trim(tag);
    for (auto& c : clean) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (clean.empty() || clean.size() > 32 || clean.find(',') != std::string::npos || !text(clean, false))
      invalid("A tag is 1 to 32 bytes of text without commas");
    if (std::find(a.tags.begin(), a.tags.end(), clean) == a.tags.end()) a.tags.push_back(std::move(clean));
  }
  if (a.tags.size() > 8) invalid("A trade takes at most eight tags");
  // The journal records UTF-8 only; reject anything else before it could fail a commit.
  try { (void)Json{{"note", a.note}, {"tags", a.tags}}.dump(); }
  catch (const Json::exception&) { invalid("Notes and tags must be UTF-8 text"); }
  return a;
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
  /// Whether `state` is the one last recorded: a transaction that changed nothing.
  [[nodiscard]] bool recorded(const Json& state) const { return base_ && *base_ == state; }
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

/// state_delta of one field's JSON, before and after, or nothing when it is
/// equal. History containers write out and compare only the subtrees they do not
/// share: a transaction copies the account, so the rest is untouched.
template <class T>
std::optional<Json> field_change(const T& before, const T& after) {
  const Json a = before, b = after;
  if (a == b) return std::nullopt;
  return detail::state_delta(a, b);
}
template <class T, std::size_t N>
std::optional<Json> field_change(const SharedVector<T, N>& before, const SharedVector<T, N>& after) {
  if (before.same(after)) return std::nullopt;
  const auto common = std::min(before.size(), after.size());
  Json changes = Json::object();
  std::size_t changed = 0;
  SharedVector<T, N>::compare(before, after, [&](std::size_t i, const T& old, const T& now) {
    const Json a = old, b = now;
    if (a == b) return;
    changes[std::to_string(i)] = detail::state_delta(a, b);
    ++changed;
  });
  if (changed == 0 && before.size() == after.size()) return std::nullopt;
  // As state_delta: an array rewritten in place is smaller whole.
  if (changed * 2 > common && common > 0) return Json{{"v", after}};
  for (std::size_t i = common; i < after.size(); ++i) changes[std::to_string(i)] = Json{{"v", after[i]}};
  return Json{{"a", std::move(changes)}, {"n", after.size()}};
}
template <class V, std::size_t N>
std::optional<Json> field_change(const SharedMap<std::string, V, N>& before, const SharedMap<std::string, V, N>& after) {
  if (before.same(after)) return std::nullopt;
  Json changes = Json::object();
  Json removed = Json::array();
  SharedMap<std::string, V, N>::compare(before, after,
      [&](const std::string& key, const V& value) { changes[key] = Json{{"v", value}}; },
      [&](const std::string& key) { removed.push_back(key); },
      [&](const std::string& key, const V& old, const V& now) {
        const Json a = old, b = now;
        if (a != b) changes[key] = detail::state_delta(a, b);
      });
  if (changes.empty() && removed.empty()) return std::nullopt;
  Json node{{"o", std::move(changes)}};
  if (!removed.empty()) node["d"] = std::move(removed);
  return node;
}
template <class K>
std::optional<Json> field_change(const SharedSet<K>& before, const SharedSet<K>& after) {
  if (before.same(after)) return std::nullopt;
  const auto common = std::min(before.size(), after.size());
  Json changes = Json::object();
  std::size_t changed = 0;
  SharedSet<K>::compare(before, after,
      [&](std::size_t i, const K& old, const K& now) {
        const Json a = old, b = now;
        if (a == b) return;
        changes[std::to_string(i)] = detail::state_delta(a, b);
        ++changed;
      },
      [&](std::size_t i, const K& key) { changes[std::to_string(i)] = Json{{"v", key}}; });
  if (changed == 0 && before.size() == after.size()) return std::nullopt;
  if (changed * 2 > common && common > 0) return Json{{"v", after}};
  return Json{{"a", std::move(changes)}, {"n", after.size()}};
}
/// state_delta(Json(before), Json(after)), found field by field without writing
/// out either state: the change a record carries. Equal states give {"o": {}}.
Json state_change(const State& before, const State& after) {
  Json changes = Json::object();
#define OPENPORT_STATE_CHANGE(field) \
  if (auto change = field_change(before.field, after.field)) changes[#field] = std::move(*change);
  OPENPORT_STATE_FIELDS(OPENPORT_STATE_CHANGE)
#undef OPENPORT_STATE_CHANGE
  return Json{{"o", std::move(changes)}};
}
/// When set, each record's change is also found by writing out both states,
/// and a difference throws (OPENPORT_VERIFY_JOURNAL).
const bool check_changes = std::getenv("OPENPORT_VERIFY_JOURNAL") != nullptr;
/// StateRecorder's records for a live session, from the committed state and the
/// next one instead of their JSON; a checkpoint writes the whole state.
class ChangeRecorder {
 public:
  /// Whether an earlier record wrote the committed state, to record changes from.
  [[nodiscard]] bool based() const { return based_; }
  /// Records `next` into `payload`: `change` (its change from the committed state,
  /// found before the version advanced) with the new version, or the whole state.
  void add(Json& payload, const State& next, std::optional<Json> change) {
    if (based_ && change && since_ + 1 < kCheckpointEvery) {
      auto delta = std::move(*change);
      delta["o"]["version"] = Json{{"v", next.version}};
      if (delta.dump().size() * 2 <= checkpoint_bytes_) {
        payload["delta"] = std::move(delta);
        ++since_;
        return;
      }
    }
    Json whole = next;
    checkpoint_bytes_ = whole.dump().size();
    payload["state"] = std::move(whole);
    based_ = true;
    since_ = 0;
  }
 private:
  bool based_ = false;
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
      validate_guardrails(c.guardrails);
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
  std::string actor = "system";
  std::shared_ptr<Journal> journal;
  std::shared_ptr<const TradingSnapshot> snapshot;
  bool stopped = false;
  /// A new session's first record, and the first after recovery, are checkpoints.
  ChangeRecorder recorder;
  /// Whether moving the clock alone to `time` could change anything. Flat with
  /// no open orders, and once the attempt has its start time, time drives no
  /// rule: no DAY or expiry cancellation, trigger, mark, freshness flag, loss
  /// or evaluation change. Rollover is its own command.
  bool idle(Timestamp time) const {
    const auto& s = state;
    return !stopped && time >= s.time && s.ledger.positions().empty() && s.ledger.stocks().empty() &&
           s.evaluation.started > 0 && s.evaluation.cycle_started > 0 &&
           s.guardrails.cooldown_until <= s.time &&
           open_ids(s).empty();
  }

  CommandResult transact(Timestamp time, std::string_view type,
                         const std::function<CommandResult(State&, Events&)>& action) {
    if (stopped) throw TradingError(Reason::JOURNAL_IO, "Trading stopped after journal failure; recover first");
    State next = state;
    Events events;
    advance(next, time, events);
    next.actor = actor;
    auto result = action(next, events);
    next.actor = "system";
    monitor_loss(next, events);
    monitor_rules(next, events);
    detail::update_reviews(next);
    reindex(next);
    std::optional<Json> change;
    if (journal && recorder.based()) {
      change = state_change(state, next);
      if (check_changes && detail::state_delta(Json(state), Json(next)) != *change)
        throw std::logic_error("The journal change differs from a comparison of both states");
      // A transaction that changed nothing, as a batch at the same market time can,
      // leaves no record, version or publication; a rejection is still recorded.
      if (events.empty() && result.decision.ok() && change->at("o").empty()) {
        result.account_version = state.version;
        return result;
      }
    }
    if (next.version == std::numeric_limits<std::uint64_t>::max())
      throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Account version exhausted");
    ++next.version;
    auto publication = std::make_shared<TradingSnapshot>(snapshot_of(next));
    if (journal) {
      // Schema 3 records the state as a change from the record before, with
      // checkpoints, and no snapshot: recovery derives it from the state.
      // Tick policy v2 extends index-v1 with equity and ETF classes.
      Json payload{{"schema", 3}, {"tick_policy", "v2"}, {"actor", actor}, {"events", events}, {"decision", result.decision}};
      recorder.add(payload, next, std::move(change));
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
TradingSession::TradingSession(SessionConfig config, Timestamp time, std::shared_ptr<Journal> journal, std::string actor)
    : impl_(std::make_unique<Impl>()) {
  validate_limits(config.limits);
  validate_scenarios(config.scenarios);
  validate_rules(config.rules);
  validate_guardrails(config.guardrails);
  if (time < 0) throw TradingError(Reason::INVALID_TIME, "Negative session time");
  if (config.fee_per_contract < Money{}) throw TradingError(Reason::INVALID_MONEY, "Fee cannot be negative");
  if (journal && journal->sequence() != 0) throw TradingError(Reason::JOURNAL_CORRUPT, "Use recover for a nonempty journal");
  impl_->actor = std::move(actor);
  impl_->state.config = std::move(config);
  impl_->state.time = time;
  impl_->state.day = md::trading_date(time);
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
void TradingSession::set_actor(std::string actor) { impl_->actor = std::move(actor); }
void TradingSession::record_input(std::string_view input, Timestamp time) {
  const auto data = Json::parse(input);
  if (!data.is_object()) throw std::invalid_argument("Run input must be an object");
  impl_->transact(time, "run_input", [&](State&, Events& events) {
    event(events, "run_input", data);
    return CommandResult{};
  });
}
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
  // A client retrying an order it may have lost the answer to gets that answer,
  // not a second order or a duplicate-key rejection; other terms still reject.
  if (!impl_->stopped) {
    const auto& state = impl_->state;
    if (const auto named = state.clients.find(request.client_order_id); named != state.clients.end()) {
      const auto& first = state.orders.at(static_cast<std::size_t>(named->second - 1));
      if (first.request == request)
        return CommandResult{first.status == OrderStatus::Rejected ? first.reason : Decision{}, first.id, state.version, true};
    }
  }
  return impl_->transact(time, "submit", [&](State& s, Events& events) {
    monitor_loss(s, events);
    return place(s, std::move(request), time, rejection, events);
  });
}
BreachRisk TradingSession::breach(const std::map<std::string, double>& close_variances) const {
  return breach_of(impl_->state, close_variances);
}
OrderPreview TradingSession::preview(const OrderRequest& request, Timestamp time, double floor_share,
    Decision rejection, const std::map<std::string, double>& close_variances, const PreviewMarket& market) const {
  if (!std::isfinite(floor_share) || floor_share <= 0 || floor_share > 1)
    throw TradingError(Reason::INVALID_ORDER, "floor_share must be greater than zero and at most one");
  // Submit answers an identical retry before advancing market time. It cannot
  // create another fill or reserve more buying power, even if the feed changed.
  if (!impl_->stopped) {
    const auto& state = impl_->state;
    const auto named = state.clients.find(request.client_order_id);
    const auto* first = named == state.clients.end() ? nullptr : &state.orders.at(static_cast<std::size_t>(named->second - 1));
    if (first && first->request == request) {
      const auto snapshot = snapshot_of(state);
      OrderPreview retry;
      retry.decision = first->status == OrderStatus::Rejected ? first->reason : Decision{};
      retry.buying_power_before = snapshot.buying_power.available;
      retry.buying_power_after = retry.buying_power_before;
      retry.exposure_change = Exposure{};
      retry.breach = breach_of(state, close_variances);
      return retry;
    }
  }
  State before = impl_->state;
  for (const auto& contract : market.contracts) {
    const auto symbol = contract.osi_symbol();
    if (!eligible(contract).ok()) continue;
    if (const auto saved = before.contracts.find(symbol); saved != before.contracts.end() && Json(saved->second) != Json(contract))
      throw TradingError(Reason::INVALID_CONTRACT, "Preview definition conflicts with registered terms");
    before.contracts[symbol] = contract;
  }
  for (const auto& quote : market.quotes) {
    if (!before.contracts.contains(quote.symbol) || quote.time > time || quote.time < 0) continue;
    const auto prior = before.books.find(quote.symbol);
    if (prior != before.books.end() && prior->second.quote.observation == quote.observation) {
      auto& book = before.books[quote.symbol];
      book.quote = quote;
    } else {
      before.books[quote.symbol] = {quote, valid_quote(quote) ? quote.bid_size : 0, valid_quote(quote) ? quote.ask_size : 0};
    }
    if (markable_quote(quote)) before.marks[quote.symbol] = {mark_of(quote), quote.time};
  }
  for (const auto& valuation : market.valuations)
    if (before.contracts.contains(valuation.symbol) && valuation.time <= time && valuation.time >= 0)
      before.valuations[valuation.symbol] = valuation;
  Events ignored;
  advance(before, time, ignored);
  monitor_loss(before, ignored);
  auto projection = project_order(before, request, rejection);
  auto result = projection.result;
  if (impl_->stopped) result.decision = failure(Reason::JOURNAL_IO, "Trading stopped after journal failure");
  result.breach = breach_of(projection.projected, close_variances);
  const auto snapshot = snapshot_of(before);
  std::optional<Money> room;
  if (before.config.rules.max_drawdown > Money{}) room = snapshot.equity - before.evaluation.floor;
  if (snapshot.soft_floor) room = room ? std::min(*room, snapshot.equity - *snapshot.soft_floor) : snapshot.equity - *snapshot.soft_floor;
  std::map<Quantity, OrderPreview> sized_previews;
  const auto sized_preview = [&](Quantity quantity) -> const OrderPreview& {
    const auto saved = sized_previews.find(quantity);
    if (saved != sized_previews.end()) return saved->second;
    auto sized = request;
    sized.quantity = quantity;
    OrderPreview value;
    try { value = project_order(before, sized, rejection).result; }
    catch (const TradingError& error) { value.decision = failure(error.code(), error.what()); }
    return sized_previews.emplace(quantity, std::move(value)).first->second;
  };
  const auto fits_limits = [&](Quantity quantity) {
    const auto& value = sized_preview(quantity);
    return value.decision.ok() && value.buying_power_after && value.max_loss;
  };
  // Convert the requested share once to millionths; floor sizing then compares
  // fixed-point dollars. Touching either floor is never an admissible size.
  const auto share = static_cast<std::int64_t>(std::floor(floor_share * 1'000'000));
  const auto fits_floor = [&](Quantity quantity) {
    const auto& loss = sized_preview(quantity).max_loss;
    return loss && (!room || (room->micros() > 0 && *loss <= room->prorate(share, 1'000'000) && *loss < *room));
  };
  Quantity upper = before.config.limits.max_order_contracts;
  for (const auto& leg : request.legs) {
    if (leg.ratio <= 0) { upper = 0; break; }
    upper = std::min(upper, before.config.limits.max_order_contracts / leg.ratio);
  }
  if (upper < 1 || !fits_limits(1) || impl_->stopped) return result;
  Quantity low = 1, high = upper;
  // Pre-trade reservations include both the held position and the candidate,
  // so increasing size cannot repair an already excessive reachable exposure.
  while (low < high) {
    const auto middle = low + (high - low) / 2 + (high - low) % 2;
    if (fits_limits(middle)) low = middle; else high = middle - 1;
  }
  upper = low;
  // Free buying power can improve while a hedge closes shorts, then decline as
  // it starts a long position. Find its feasible interval before sizing to loss.
  low = 1; high = upper;
  while (low < high) {
    const auto middle = low + (high - low) / 2;
    if (*sized_preview(middle).buying_power_after < *sized_preview(middle + 1).buying_power_after) low = middle + 1;
    else high = middle;
  }
  const auto best_power = low;
  if (*sized_preview(best_power).buying_power_after < Money{}) return result;
  low = 1; high = best_power;
  while (low < high) {
    const auto middle = low + (high - low) / 2;
    if (*sized_preview(middle).buying_power_after >= Money{}) high = middle; else low = middle + 1;
  }
  const auto lower = low;
  low = best_power; high = upper;
  while (low < high) {
    const auto middle = low + (high - low) / 2 + (high - low) % 2;
    if (*sized_preview(middle).buying_power_after >= Money{}) low = middle; else high = middle - 1;
  }
  upper = low;
  // Scenario losses can first fall as an order hedges the book, then rise.
  // Their maximum of linear per-cell losses is convex: find its minimum before
  // searching the upper feasible edge, rather than assuming one unit fits.
  low = lower; high = upper;
  while (low < high) {
    const auto middle = low + (high - low) / 2;
    const auto& a = sized_preview(middle).max_loss;
    const auto& b = sized_preview(middle + 1).max_loss;
    if (a && b && *a > *b) low = middle + 1; else high = middle;
  }
  if (!fits_floor(low)) return result;
  high = upper;
  while (low < high) {
    const auto middle = low + (high - low) / 2 + (high - low) % 2;
    if (fits_floor(middle)) low = middle; else high = middle - 1;
  }
  result.max_units = low;
  return result;
}
CommandResult TradingSession::cancel(OrderId id, Timestamp time) {
  return impl_->transact(time, "cancel", [&](State& s, Events& events) {
    if (id == 0 || id > s.orders.size()) return CommandResult{failure(Reason::UNKNOWN_ORDER, "Unknown order ID"), {}, 0};
    if (!s.orders.at(static_cast<std::size_t>(id - 1)).open())
      return CommandResult{failure(Reason::ORDER_TERMINAL, "Order is already terminal"), id, 0};
    cancel_order(s.orders.mut(id - 1), failure(Reason::USER_CANCEL, "Cancelled by caller"), events);
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
    for (const auto id : open_ids(s))
      if (const auto& o = s.orders[id - 1]; o.open() && (!underlying || underlying_of(s, o) == *underlying))
        cancel_order(s.orders.mut(id - 1), failure(Reason::USER_CANCEL, "Cancelled by caller"), events);
    return CommandResult{};
  });
}
CommandResult TradingSession::close_positions(std::optional<std::string> underlying, Timestamp time,
                                              const std::map<std::string, Decision>& rejections) {
  return impl_->transact(time, "close_positions", [&](State& s, Events& events) {
    monitor_loss(s, events);
    const auto in_scope = [&](const std::string& name) { return !underlying || name == *underlying; };
    for (const auto id : open_ids(s))
      if (const auto& o = s.orders[id - 1]; o.open() && in_scope(underlying_of(s, o)))
        cancel_order(s.orders.mut(id - 1), failure(Reason::USER_CANCEL, "Cancelled to close positions"), events);
    std::vector<std::pair<std::string, Quantity>> closing;
    // Expired contracts cannot trade; they close at settlement.
    for (const auto& [symbol, position] : s.ledger.positions()) {
      const auto& contract = s.contracts.at(symbol);
      if (position.quantity != 0 && in_scope(contract.underlying) && s.time < contract.expiry_time())
        closing.emplace_back(symbol, position.quantity);
    }
    // Shorts first: buying one back never uncovers another leg.
    std::stable_partition(closing.begin(), closing.end(), [](const auto& p) { return p.second < 0; });
    // A client ID the trader already used stays theirs: take the next free number.
    const auto prefix = "openport-close-" + std::to_string(s.version + 1) + "-";
    std::size_t count = 0;
    const auto client_id = [&] {
      auto id = prefix + std::to_string(++count);
      while (s.clients.contains(id)) id = prefix + std::to_string(++count);
      return id;
    };
    for (const auto& [symbol, quantity] : closing) {
      OrderRequest request;
      request.client_order_id = client_id();
      request.symbol = symbol;
      request.side = quantity > 0 ? Side::Sell : Side::Buy;
      request.type = OrderType::Market;
      request.tif = TimeInForce::Ioc;
      request.quantity = magnitude(quantity);
      const auto gate = rejections.find(s.contracts.at(symbol).underlying);
      place(s, std::move(request), time, gate == rejections.end() ? Decision{} : gate->second, events);
    }
    // Shares close at the underlying's fresh price in the regular session.
    std::vector<std::pair<std::string, Quantity>> stocks;
    if (account_check(s, true).ok())
      for (const auto& [symbol, stock] : s.ledger.stocks())
        if (in_scope(symbol) && !rejections.contains(symbol)) stocks.emplace_back(symbol, stock.shares);
    for (const auto& [symbol, shares] : stocks) close_shares(s, symbol, shares, StockSource::Trade, events);
    return CommandResult{};
  });
}
CommandResult TradingSession::on_quotes(const std::vector<QuoteObservation>& quotes,
    const std::vector<Valuation>& valuations, Timestamp time, const std::vector<StockPrice>& stocks) {
  // An empty batch on an idle account only moves the clock: not a transaction,
  // so nothing is journaled. The next transaction advances the clock itself.
  if (quotes.empty() && valuations.empty() && stocks.empty() && impl_->idle(time)) return CommandResult{{}, {}, impl_->state.version};
  return impl_->transact(time, "market", [&](State& s, Events& events) {
    std::set<std::string> seen;
    std::set<std::string> changed;
    std::set<std::string> offered_again;
    for (const auto& quote : quotes) {
      if (!s.contracts.contains(quote.symbol)) throw TradingError(Reason::UNKNOWN_CONTRACT, "Quote references unregistered OSI");
      if (quote.time < 0 || quote.time > time) throw TradingError(Reason::INVALID_TIME, "Quote is future-dated or negative");
      if (!seen.insert(quote.symbol).second) throw TradingError(Reason::INVALID_QUOTE, "One observation per contract per batch is required");
      auto& book = s.books[quote.symbol];
      if (quote.observation == book.quote.observation && quote.time >= book.quote.time) {
        // The same quote, confirmed current at a later time: it and its mark stay
        // fresh, and it keeps what is left of its displayed size.
        if (quote.time > book.quote.time) {
          book.quote.time = quote.time;
          if (markable_quote(book.quote)) s.marks[quote.symbol] = {mark_of(book.quote), quote.time};
        }
        offered_again.insert(quote.symbol);
        continue;
      }
      if (quote.observation <= book.quote.observation || quote.time < book.quote.time) continue;
      book = {quote, valid_quote(quote) ? quote.bid_size : 0, valid_quote(quote) ? quote.ask_size : 0};
      changed.insert(quote.symbol);
      if (markable_quote(quote) && time - quote.time <= s.config.limits.max_quote_age)
        s.marks[quote.symbol] = {mark_of(quote), quote.time};
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
    seen.clear();
    for (const auto& price : stocks) {
      if (price.symbol.empty() || price.price <= Money{}) throw TradingError(Reason::INVALID_QUOTE, "A stock price needs a symbol and a positive price");
      if (price.time < 0 || price.time > time) throw TradingError(Reason::INVALID_TIME, "Stock price is future-dated or negative");
      if (!seen.insert(price.symbol).second) throw TradingError(Reason::INVALID_QUOTE, "One price per underlying per batch is required");
      const auto prior = s.stock_marks.find(price.symbol);
      if (prior != s.stock_marks.end() && price.time < prior->second.time) continue;
      s.stock_marks[price.symbol] = {price.price, price.time};
    }
    detail::update_reviews(s);
    monitor_loss(s, events);
    monitor_rules(s, events);
    // Invalid quotes provide no liquidity. Keep orders until a new valid quote
    // permits the fill-time risk check (or a clock/kill command cancels them).
    for (auto it = changed.begin(); it != changed.end();) {
      if (!quote_check(s, *it).ok()) it = changed.erase(it); else ++it;
    }
    // Offered again, a quote's remaining displayed size can fill the orders that
    // a data gap held back when it was new.
    for (const auto id : open_ids(s)) {
      const auto& o = s.orders[id - 1];
      if (o.status == OrderStatus::Armed) continue;
      for (const auto& symbol : order_symbols(o.request))
        if (offered_again.contains(symbol) && quote_check(s, symbol).ok()) changed.insert(symbol);
    }
    match_symbols(s, changed, events);
    // Triggers read the batch's books and valuations after resting orders match.
    check_triggers(s, events);
    return CommandResult{};
  });
}
CommandResult TradingSession::set_limits(Limits limits, Timestamp time) {
  validate_limits(limits);
  return impl_->transact(time, "limit_change", [&](State& s, Events& events) {
    const auto effective = s.config.rules.evaluation() ? tightened_limits(s.config.limits, limits) : limits;
    s.pending_limits = Json(effective) != Json(limits) ? std::optional(limits) : std::nullopt;
    s.config.limits = effective;
    if (s.limits_revision == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Limits revision exhausted");
    ++s.limits_revision;
    event(events, "limit_change", Json{{"effective", s.config.limits}, {"pending", s.pending_limits}});
    monitor_loss(s, events);
    for (const auto id : open_ids(s)) {
      const auto& order = s.orders[id - 1];
      if (!order.open()) continue;
      auto d = order.system || order.role != OrderRole::Normal ? system_check(s, order) : order_check(s, order);
      if (persistent(order) && (data_gap(d.code) || d.code == Reason::SESSION_CLOSED || d.code == Reason::LIMIT_ONLY)) continue;
      if (!d.ok()) {
        d.message = std::string(to_string(d.code)) + ": " + d.message;
        d.code = Reason::RISK_CHANGED;
        cancel_order(s.orders.mut(id - 1), d, events);
      }
    }
    return CommandResult{};
  });
}
CommandResult TradingSession::set_guardrails(Guardrails guardrails, Timestamp time) {
  validate_guardrails(guardrails);
  return impl_->transact(time, "guardrail_change", [&](State& s, Events& events) {
    if (guardrails.soft_floor > Money{} && s.config.rules.max_drawdown > Money{} && guardrails.soft_floor <= s.evaluation.floor)
      throw TradingError(Reason::INVALID_LIMITS, "Soft floor equity must be above the plan floor");
    const auto effective = tightened_guardrails(s.config.guardrails, guardrails);
    if (s.guardrails.cooldown_until > s.time && effective.cooldown_minutes > s.config.guardrails.cooldown_minutes) {
      const auto extension = (effective.cooldown_minutes - s.config.guardrails.cooldown_minutes) * md::kNanosPerMinute;
      if (s.guardrails.cooldown_until > std::numeric_limits<Timestamp>::max() - extension)
        throw TradingError(Reason::INVALID_TIME, "Cooldown time exceeds timestamp range");
      s.guardrails.cooldown_until += extension;
    }
    s.pending_guardrails = effective != guardrails ? std::optional(guardrails) : std::nullopt;
    s.config.guardrails = effective;
    if (s.limits_revision == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Limits revision exhausted");
    ++s.limits_revision;
    event(events, "guardrail_change", Json{{"effective", effective}, {"pending", s.pending_guardrails}});
    return CommandResult{};
  });
}
CommandResult TradingSession::trip_kill(std::string reason, Timestamp time) {
  require_reason(reason);
  return impl_->transact(time, "kill_trip", [&](State& s, Events& events) {
    trip(s, reason, events);
    s.kill_reason = reason;
    s.guardrails.owns_kill = false;
    return CommandResult{};
  });
}
CommandResult TradingSession::reset_kill(std::string reason, Timestamp time) {
  require_reason(reason);
  return impl_->transact(time, "kill_reset", [&](State& s, Events& events) {
    const auto personal = guardrail_reason(s);
    if (personal != Reason::NONE) return CommandResult{failure(personal, "Personal guardrail remains active until its market-time expiry"), {}, 0};
    s.kill = false;
    s.kill_reason.clear();
    event(events, "kill_reset", Json{{"reason", reason}});
    monitor_loss(s, events);
    if (s.kill) return CommandResult{loss_check(s, measure(s)), {}, 0};
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
    // The last stretch ends at intrinsic value, at expiry with the settlement
    // as the underlying's price and the volatility unchanged.
    std::optional<Valuation> at_expiry;
    if (const auto r = s.references.find(symbol); r != s.references.end() && r->second.valuation) {
      at_expiry = *r->second.valuation;
      at_expiry->spot = settlement.dollars();
      at_expiry->years = 0;
    }
    end_stretch(s, symbol, intrinsic, at_expiry ? &*at_expiry : nullptr);
    s.ledger.settle(symbol, intrinsic);
    s.settled.insert(symbol);
    s.settling[symbol] = settlement;
    s.closures.push_back({symbol, quantity, intrinsic, time, ClosureKind::Settlement, s.fills.size()});
    sync_exits(s, symbol, events);
    event(events, "settlement", Json{{"symbol", symbol}, {"reference", settlement}, {"intrinsic", intrinsic}});
    // American equity and ETF options a cent or more in the money are exercised
    // or assigned: settled at intrinsic value, they deliver shares at the
    // settlement price, which together cost the strike.
    if (physical(it->second) && intrinsic >= Money::from_micros(10'000)) {
      const auto underlying = it->second.underlying;
      const auto shares = delivered(it->second, quantity);
      if (const auto mark = s.stock_marks.find(underlying); mark == s.stock_marks.end() || mark->second.time <= time)
        s.stock_marks[underlying] = {settlement, time};
      trade_shares(s, underlying, shares, settlement, StockSource::Delivery, symbol);
      event(events, "delivery", Json{{"symbol", symbol}, {"underlying", underlying}, {"shares", shares}, {"price", settlement}});
    }
    return CommandResult{};
  });
}
namespace {
/// Overnight, short American equity and ETF options that the market values below
/// their exercise value are assigned: a holder then does better exercising than
/// selling, as with a deep put or a call before its dividend. Each is assigned in
/// full, bought back at intrinsic value, and delivers shares at the underlying's
/// price, together the strike. Options that expire today settle instead.
/// splitmix64, so a replay draws the same assignments on every platform.
std::uint64_t mix(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
/// How many of `contracts` short contracts holders' exercises reach: each with even
/// odds, as the OCC allocates exercises at random, drawn from the attempt, the
/// contract and the date so a replay assigns the same ones.
Quantity assigned_contracts(const State& s, const std::string& symbol, Quantity contracts) {
  auto seed = mix(static_cast<std::uint64_t>(s.evaluation.started));
  for (const unsigned char c : symbol + " " + md::format_date(s.day)) seed = mix(seed ^ c);
  Quantity assigned = 0;
  for (Quantity i = 0; i < contracts; ++i) assigned += static_cast<Quantity>((seed = mix(seed)) >> 63);
  return assigned;
}
void assign_early(State& s, const std::vector<Dividend>& dividends, Events& events) {
  std::vector<std::string> shorts;
  for (const auto& [symbol, position] : s.ledger.positions()) {
    const auto& c = s.contracts.at(symbol);
    if (position.quantity < 0 && physical(c) && s.time < c.expiry_time()) shorts.push_back(symbol);
  }
  for (const auto& symbol : shorts) {
    const auto contract = s.contracts.at(symbol);
    const auto mark = s.marks.find(symbol);
    const auto price = stock_price(s, contract.underlying);
    if (mark == s.marks.end() || !price) continue;
    const Money strike = Money::from_double(contract.strike);
    const bool call = contract.type == pricing::OptionType::Call;
    const Money intrinsic = std::max(Money{}, call ? *price - strike : strike - *price);
    if (intrinsic < Money::from_micros(10'000)) continue;
    // Holders exercise an option trading below its exercise value, and a call
    // whose time value is less than a dividend going ex on the new day.
    Money dividend;
    for (const auto& d : dividends)
      if (call && d.symbol == contract.underlying) dividend = dividend + d.per_share;
    const char* reason = mark->second.price < intrinsic ? "below_intrinsic"
                       : mark->second.price - intrinsic < dividend ? "dividend" : nullptr;
    if (!reason) continue;
    const auto short_contracts = -held(s, symbol);
    const auto contracts = assigned_contracts(s, symbol, short_contracts);
    if (contracts == 0) continue;
    const auto shares = delivered(contract, -contracts);
    fill_position(s, symbol, contracts, intrinsic, Money{});
    s.closures.push_back({symbol, -contracts, intrinsic, s.time, ClosureKind::Assignment, s.fills.size()});
    trade_shares(s, contract.underlying, shares, *price, StockSource::Assignment, symbol);
    sync_exits(s, symbol, events);
    event(events, "assignment", Json{{"symbol", symbol}, {"contracts", contracts}, {"of", short_contracts}, {"reason", reason},
                                     {"intrinsic", intrinsic}, {"mark", mark->second.price},
                                     {"underlying", contract.underlying}, {"shares", shares}, {"price", *price}});
  }
}
/// On an ex-date the shares held into it are paid the dividend, and short shares
/// pay it: once per symbol and date, into the new day's P&L as other.
void pay_dividends(State& s, const std::vector<Dividend>& dividends, Events& events) {
  for (const auto& d : dividends) {
    const auto shares = shares_held(s, d.symbol);
    const bool paid = std::any_of(s.dividends.begin(), s.dividends.end(),
        [&](const DividendPayment& p) { return p.symbol == d.symbol && p.ex_date == d.ex_date; });
    if (shares == 0 || paid || d.per_share <= Money{}) continue;
    const Money amount = d.per_share * shares;
    s.ledger.receive_dividend(d.symbol, amount);
    s.explained[d.symbol].other += amount.dollars();
    s.dividends.push_back({d.symbol, d.ex_date, d.per_share, shares, amount, s.time});
    event(events, "dividend", Json{{"symbol", d.symbol}, {"ex_date", d.ex_date}, {"per_share", d.per_share},
                                   {"shares", shares}, {"amount", amount}});
  }
}
}  // namespace
namespace {
std::string close_key(const std::string& underlying, md::Date date) { return underlying + " " + md::format_date(date); }
}  // namespace
CommandResult TradingSession::record_close(const std::string& underlying, md::Date date, Money price, Timestamp print_time,
                                           Timestamp time) {
  if (underlying.empty() || price <= Money{} || !md::valid_date(date))
    throw TradingError(Reason::INVALID_SETTLEMENT, "A closing print needs an underlying, a valid date and a positive price");
  return impl_->transact(time, "closing_print", [&](State& s, Events& events) {
    auto& print = s.closing_prints[close_key(underlying, date)];
    if (print.price != price) {
      print = ClosingPrint{price, print_time};
      event(events, "closing_print", Json{{"underlying", underlying}, {"date", date}, {"price", price}, {"time", print_time}});
    }
    return CommandResult{};
  });
}
std::optional<ClosingPrint> TradingSession::closing_print(const std::string& underlying, md::Date date) const {
  const auto& prints = impl_->state.closing_prints;
  const auto it = prints.find(close_key(underlying, date));
  return it == prints.end() ? std::nullopt : std::optional(it->second);
}
CommandResult TradingSession::roll_day(Timestamp time, const std::vector<Dividend>& dividends) {
  return impl_->transact(time, "day_rollover", [&](State& s, Events& events) {
    const auto day = md::trading_date(time);
    if (day <= s.day) return CommandResult{failure(Reason::INVALID_TIME, "Rollover requires a later trading date"), {}, 0};
    monitor_loss(s, events);
    const auto snapshot = snapshot_of(s);
    if (!snapshot.valuation_complete) return CommandResult{failure(Reason::STALE_QUOTE, "Rollover requires complete marked equity"), {}, 0};
    // Close the finished day. An end-of-day floor ratchets only here, from the
    // last fully marked equity observed on that date; breaches are still checked
    // on every transaction by monitor_rules.
    auto& e = s.evaluation;
    const auto& rules = s.config.rules;
    evaluation_rollover(e, rules);
    // A placeholder date from before the attempt started is not a trading day.
    // On a funded account, a day with enough net realised profit counts once,
    // toward the payout cycle in progress when it closes.
    if (e.started > 0 && e.day >= md::trading_date(e.started)) {
      const auto realised = net_realised(s) - e.day_open_realised;
      const auto& payouts = rules.payouts;
      const bool qualifying = rules.phase == Phase::Funded && e.status == EvaluationStatus::Active &&
          payouts.qualifying_days > 0 && realised >= payouts.qualifying_profit && realised > Money{};
      if (qualifying) ++e.qualifying_days;
      e.days.push_back({e.day, e.day_open_equity, e.day_close_equity, e.peak, e.floor, realised, qualifying, snapshot.attribution, e.day_low_equity, e.day_high_equity, e.day_low_at, e.day_high_at});
      event(events, "evaluation_day", e.days.back());
    }
    if (s.pending_limits || s.pending_guardrails) {
      if (s.limits_revision == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Limits revision exhausted");
      if (s.pending_limits) { s.config.limits = *s.pending_limits; s.pending_limits.reset(); }
      if (s.pending_guardrails) { s.config.guardrails = *s.pending_guardrails; s.pending_guardrails.reset(); }
      ++s.limits_revision;
      s.pending_applied_at = s.time;
      event(events, "pending_limits_applied", Json{{"limits", s.config.limits}, {"guardrails", s.config.guardrails}});
    }
    const bool personal_latch = s.guardrails.owns_kill;
    const auto cooldown_until = s.guardrails.cooldown_until;
    s.guardrails = {};
    s.guardrails.owns_kill = personal_latch;
    if (cooldown_until > s.time) s.guardrails.cooldown_until = cooldown_until;
    refresh_guardrail_latch(s, events);
    e.day_low_equity = snapshot.equity; e.day_high_equity = snapshot.equity;
    e.day_low_at = s.time; e.day_high_at = s.time;
    e.day_open_realised = net_realised(s);
    e.day = day;
    e.day_open_equity = snapshot.equity;
    e.day_close_equity = snapshot.equity;
    s.start_equity = snapshot.equity;
    s.day = day;
    // The new day's P&L by Greek runs from these marks.
    s.explained.clear();
    s.references.clear();
    for (const auto& [symbol, position] : s.ledger.positions()) start_stretch(s, symbol);
    for (const auto& [symbol, stock] : s.ledger.stocks()) start_stock_stretch(s, symbol);
    event(events, "day_rollover", Json{{"day", day}, {"equity", s.start_equity}});
    // Assignments arrive overnight, so the new day takes them, and then the
    // ex-date's dividends pay the shares held into it.
    assign_early(s, dividends, events);
    pay_dividends(s, dividends, events);
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
      e.floor = evaluation_floor(rules, e.peak, e.floor_locked);
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
    for (const auto id : open_ids(s)) cancel_order(s.orders.mut(id - 1), failure(Reason::ACCOUNT_RESET, reason), events);
    for (const auto& p : snapshot.positions) {
      const auto& position = p.position;
      s.closures.push_back({position.contract.osi_symbol(), position.quantity,
                            p.mark ? *p.mark : average_unit_price(position), s.time, ClosureKind::Reset, s.fills.size()});
    }
    // Shares leave with the old ledger too, at their mark or else their cost.
    for (const auto& held : snapshot.stocks) {
      const auto& p = held.position;
      const auto magnitude = p.shares < 0 ? -p.shares : p.shares;
      const auto price = held.mark ? *held.mark : (p.shares < 0 ? -p.basis : p.basis).prorate(1, magnitude);
      s.stock_fills.push_back({s.stock_fills.size() + 1, p.symbol, -p.shares, price, s.time, StockSource::Reset, {}});
    }
    const auto& e = s.evaluation;
    s.attempts.push_back({e.attempt, s.config.rules.plan, e.started, s.time, e.starting_balance, snapshot.equity,
                          e.status, e.decision, e.first_order, e.first_fill});
    const auto attempt = e.attempt + 1;
    s.config.initial_cash = initial_cash;
    s.config.rules = std::move(rules);
    s.ledger = Ledger(initial_cash);
    s.explained.clear();
    s.references.clear();
    s.start_equity = initial_cash;
    s.kill = false;
    s.kill_reason.clear();
    if (s.pending_limits) { s.config.limits = *s.pending_limits; s.pending_limits.reset(); ++s.limits_revision; }
    if (s.pending_guardrails) { s.config.guardrails = *s.pending_guardrails; s.pending_guardrails.reset(); ++s.limits_revision; }
    s.guardrails = {};
    s.pending_applied_at = 0;
    s.day = md::trading_date(s.time);
    s.evaluation = fresh_evaluation(s, attempt);
    event(events, "account_reset", Json{{"reason", reason}, {"attempt", attempt}, {"initial_cash", initial_cash},
                                        {"rules", s.config.rules}});
    return CommandResult{};
  });
}
std::shared_ptr<const TradingSnapshot> TradingSession::snapshot() const { return impl_->snapshot; }
std::string TradingSession::snapshot_json() const { return Json(*impl_->snapshot).dump(); }
const SessionConfig& TradingSession::config() const { return impl_->state.config; }
const Contracts& TradingSession::contracts() const { return impl_->state.contracts; }
const Valuations& TradingSession::valuations() const { return impl_->state.valuations; }
std::optional<QuoteObservation> TradingSession::quote(const std::string& symbol) const {
  const auto it = impl_->state.books.find(symbol);
  return it == impl_->state.books.end() ? std::nullopt : std::optional(it->second.quote);
}
md::Date TradingSession::trading_day() const { return impl_->state.day; }
namespace {
/// Records (or, empty, clears) a trade's note and tags under `key`.
void store_annotation(State& s, const std::string& key, Annotation annotation, Events& events) {
  event(events, "trade_annotated", Json{{"trade", key}, {"note", annotation.note}, {"tags", annotation.tags}});
  if (annotation.note.empty() && annotation.tags.empty()) {
    s.annotations.erase(key);
  } else {
    annotation.time = s.time;
    s.annotations[key] = std::move(annotation);
  }
}
}  // namespace
CommandResult TradingSession::annotate(std::uint64_t trade, std::string note, std::vector<std::string> tags, Timestamp time) {
  auto annotation = clean_annotation(std::move(note), tags);
  return impl_->transact(time, "annotate", [&](State& s, Events& events) {
    const auto trades = lifecycles(s.fills, s.closures, s.contracts);
    if (std::none_of(trades.begin(), trades.end(), [&](const Lifecycle& t) { return t.first_fill == trade; }))
      return CommandResult{failure(Reason::UNKNOWN_TRADE, "No trade opens with fill " + std::to_string(trade)), {}, 0};
    store_annotation(s, std::to_string(trade), std::move(annotation), events);
    return CommandResult{};
  });
}
CommandResult TradingSession::annotate_day(md::Date day, std::string plan, std::string review, Timestamp time) {
  if (!md::valid_date(day))
    throw TradingError(Reason::INVALID_NOTE, "A day note needs a valid YYYY-MM-DD date");
  DayNote note{clean_annotation(std::move(plan), {}).note, clean_annotation(std::move(review), {}).note, time};
  return impl_->transact(time, "day_note", [&](State& s, Events& events) {
    const auto key = md::format_date(day);
    if (note.plan.empty() && note.review.empty()) s.day_notes.erase(key);
    else s.day_notes[key] = note;
    event(events, "day_annotated", Json{{"day", key}, {"note", note}});
    return CommandResult{};
  });
}
CommandResult TradingSession::annotate_shares(std::uint64_t first_fill, std::string note, std::vector<std::string> tags, Timestamp time) {
  auto annotation = clean_annotation(std::move(note), tags);
  return impl_->transact(time, "annotate", [&](State& s, Events& events) {
    const auto trips = share_lifecycles(s.stock_fills, s.dividends);
    if (std::none_of(trips.begin(), trips.end(), [&](const ShareLifecycle& t) { return t.fills.front() == first_fill; }))
      return CommandResult{failure(Reason::UNKNOWN_TRADE, "No share trade opens with stock fill " + std::to_string(first_fill)), {}, 0};
    store_annotation(s, "s" + std::to_string(first_fill), std::move(annotation), events);
    return CommandResult{};
  });
}
CommandResult TradingSession::exercise(const std::string& symbol, Quantity contracts, Timestamp time) {
  if (contracts <= 0) throw TradingError(Reason::INVALID_ORDER, "Exercise a positive number of contracts");
  return impl_->transact(time, "exercise", [&](State& s, Events& events) {
    const auto it = s.contracts.find(symbol);
    if (it == s.contracts.end()) return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "Exercise references an unregistered OSI"), {}, 0};
    const auto contract = it->second;
    if (!physical(contract))
      return CommandResult{failure(Reason::INVALID_ORDER, "Only American equity and ETF options deliver shares; index options settle in cash"), {}, 0};
    if (s.time >= contract.expiry_time()) return CommandResult{failure(Reason::EXPIRED, "The contract has expired; settlement exercises it"), {}, 0};
    if (const auto d = account_check(s); !d.ok()) return CommandResult{d, {}, 0};
    if (held(s, symbol) < contracts) return CommandResult{failure(Reason::INVALID_ORDER, "Exercise needs that many long contracts"), {}, 0};
    if (const auto d = defined_risk_check(s, {{symbol, -contracts}},
                                          "Exercising this long would leave a short option uncovered; close the short first");
        !d.ok())
      return CommandResult{d, {}, 0};
    const auto price = stock_price(s, contract.underlying);
    if (!price) return CommandResult{failure(Reason::STALE_QUOTE, "Exercise needs a fresh price for " + contract.underlying), {}, 0};
    const Money strike = Money::from_double(contract.strike);
    const Money intrinsic = std::max(Money{}, contract.type == pricing::OptionType::Call ? *price - strike : strike - *price);
    if (intrinsic <= Money{}) return CommandResult{failure(Reason::INVALID_ORDER, "Exercise needs the option in the money"), {}, 0};
    const auto shares = delivered(contract, contracts);
    // The contracts close at intrinsic value, so any time value left is a cost,
    // and the shares change hands at the underlying's price.
    const auto apply = [&](State& t) {
      fill_position(t, symbol, -contracts, intrinsic, Money{});
      t.closures.push_back({symbol, contracts, intrinsic, t.time, ClosureKind::Exercise, t.fills.size()});
      trade_shares(t, contract.underlying, shares, *price, StockSource::Exercise, symbol);
    };
    if (s.config.rules.buying_power) {
      State projected = s;
      apply(projected);
      const auto power = buying_power(projected).total;
      if (free_power(projected) < free_power(s) && power.available < Money{})
        return CommandResult{{Reason::BUYING_POWER, "Exercise needs more buying power than the account has available",
                              (-power.available).dollars(), 0.0, contract.underlying}, {}, 0};
    }
    apply(s);
    sync_exits(s, symbol, events);
    event(events, "exercise", Json{{"symbol", symbol}, {"contracts", contracts}, {"intrinsic", intrinsic},
                                   {"underlying", contract.underlying}, {"shares", shares}, {"price", *price}});
    return CommandResult{};
  });
}
CommandResult TradingSession::trade_stock(const std::string& symbol, Quantity signed_shares, Timestamp time) {
  if (signed_shares == 0) throw TradingError(Reason::INVALID_ORDER, "Trade a nonzero number of shares");
  return impl_->transact(time, "stock_trade", [&](State& s, Events& events) {
    const auto shares = shares_held(s, symbol);
    const bool reduces = shares != 0 && (shares > 0) != (signed_shares > 0) &&
                         (shares > 0 ? -signed_shares <= shares : signed_shares <= -shares);
    if (!reduces)
      return CommandResult{failure(Reason::INVALID_ORDER, "Stock trades only reduce the shares exercise and assignment delivered"), {}, 0};
    if (const auto d = account_check(s, true); !d.ok()) return CommandResult{d, {}, 0};
    if (!md::market_session(s.time).open) return CommandResult{failure(Reason::SESSION_CLOSED, "Stock trades in the regular session"), {}, 0};
    const auto price = stock_price(s, symbol);
    if (!price) return CommandResult{failure(Reason::STALE_QUOTE, "Needs a fresh price for " + symbol), {}, 0};
    const auto realised_before = s.ledger.account().realised;
    trade_shares(s, symbol, signed_shares, *price, StockSource::Trade);
    if (s.config.guardrails.cooldown_loss > Money{} && s.ledger.account().realised - realised_before < -s.config.guardrails.cooldown_loss)
      begin_cooldown(s, events);
    event(events, "stock_trade", Json{{"symbol", symbol}, {"shares", signed_shares}, {"price", *price}});
    return CommandResult{};
  });
}
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

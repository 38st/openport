#pragma once

#include <set>
#include <nlohmann/json.hpp>
#include "openport/trading/history.hpp"
#include "openport/trading/session.hpp"

namespace nlohmann {
template <class T> struct adl_serializer<std::optional<T>> {
  static void to_json(json& j, const std::optional<T>& value) { if (value) j = *value; else j = nullptr; }
  static void from_json(const json& j, std::optional<T>& value) {
    if (j.is_null()) value.reset(); else value = j.get<T>();
  }
};
}  // namespace nlohmann
namespace openport::md {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Date, year, month, day)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OptionContract, root, underlying, expiry, strike, type, style, settlement, multiplier, standard)
}  // namespace openport::md
namespace openport::trading {
using Json = nlohmann::json;
inline void to_json(Json& j, Money m) { j = m.micros(); }
inline void from_json(const Json& j, Money& m) { m = Money::from_micros(j.get<std::int64_t>()); }
inline void to_json(Json& j, Reason r) { j = to_string(r); }
inline void from_json(const Json& j, Reason& r) {
  const auto text = j.get<std::string>();
  for (int i = 0; i <= static_cast<int>(kLastReason); ++i) {
    const auto candidate = static_cast<Reason>(i);
    if (to_string(candidate) == text) { r = candidate; return; }
  }
  throw TradingError(Reason::JOURNAL_CORRUPT, "Unknown recorded reason code");
}
/// Schema 1 records predate account rules. Their original keys stay required;
/// only keys introduced by schema 2 fall back to defaults.
template <class T> void added_field(const Json& j, const char* key, T& value) {
  if (const auto it = j.find(key); it != j.end()) it->get_to(value);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Decision, code, message, actual, limit, scope)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Trigger, source, direction, level)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExitSpec, trigger, limit_price)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Bracket, stop_loss, take_profit)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Leg, symbol, side, ratio)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(OrderRequest, client_order_id, symbol, side, type, tif, quantity, limit_price, trigger, bracket, legs, tags, note, exits_only)
inline void from_json(const Json& j, OrderRequest& r) {
  j.at("client_order_id").get_to(r.client_order_id); j.at("symbol").get_to(r.symbol); j.at("side").get_to(r.side);
  j.at("type").get_to(r.type); j.at("tif").get_to(r.tif); j.at("quantity").get_to(r.quantity);
  j.at("limit_price").get_to(r.limit_price);
  added_field(j, "trigger", r.trigger); added_field(j, "bracket", r.bracket); added_field(j, "legs", r.legs);
  added_field(j, "tags", r.tags); added_field(j, "note", r.note); added_field(j, "exits_only", r.exits_only);
}
inline void to_json(Json& j, const OrderChangeRecord& c) {
  j = Json{{"time", c.time}, {"actor", c.actor}, {"previous_quantity", c.previous_quantity}};
  if (c.quantity) j["quantity"] = *c.quantity;
  if (c.limit_price) j["limit_price"] = *c.limit_price;
  if (c.trigger_level) j["trigger_level"] = *c.trigger_level;
  if (c.previous_limit_price) j["previous_limit_price"] = *c.previous_limit_price;
  if (c.previous_trigger_level) j["previous_trigger_level"] = *c.previous_trigger_level;
  if (!c.decision.ok()) j["decision"] = c.decision;
}
inline void from_json(const Json& j, OrderChangeRecord& c) {
  j.at("time").get_to(c.time); j.at("actor").get_to(c.actor); j.at("previous_quantity").get_to(c.previous_quantity);
  added_field(j, "quantity", c.quantity); added_field(j, "limit_price", c.limit_price);
  added_field(j, "trigger_level", c.trigger_level); added_field(j, "previous_limit_price", c.previous_limit_price);
  added_field(j, "previous_trigger_level", c.previous_trigger_level); added_field(j, "decision", c.decision);
}
inline void to_json(Json& j, const Order& o) {
  j = Json{{"id", o.id}, {"request", o.request}, {"status", o.status}, {"filled_quantity", o.filled_quantity},
           {"filled_notional", o.filled_notional}, {"accepted_at", o.accepted_at}, {"day_end", o.day_end},
           {"reason", o.reason}, {"system", o.system}, {"role", o.role}, {"parent", o.parent}, {"oco", o.oco},
           {"stop_loss", o.stop_loss}, {"take_profit", o.take_profit}, {"triggered_at", o.triggered_at}, {"actor", o.actor}};
  // Only a changed order records its submitted terms; every other order keeps its bytes.
  if (o.submitted) j["submitted"] = *o.submitted;
  // Likewise an order's end and its changes, once it has them.
  if (o.ended_at != 0) j["ended_at"] = o.ended_at;
  if (!o.changes.empty()) j["changes"] = o.changes;
}
inline void from_json(const Json& j, Order& o) {
  j.at("id").get_to(o.id); j.at("request").get_to(o.request); j.at("status").get_to(o.status);
  j.at("filled_quantity").get_to(o.filled_quantity); j.at("filled_notional").get_to(o.filled_notional);
  j.at("accepted_at").get_to(o.accepted_at); j.at("day_end").get_to(o.day_end); j.at("reason").get_to(o.reason);
  added_field(j, "system", o.system); added_field(j, "role", o.role); added_field(j, "parent", o.parent);
  added_field(j, "oco", o.oco); added_field(j, "stop_loss", o.stop_loss); added_field(j, "take_profit", o.take_profit);
  added_field(j, "triggered_at", o.triggered_at); added_field(j, "actor", o.actor);
  added_field(j, "submitted", o.submitted);
  added_field(j, "ended_at", o.ended_at); added_field(j, "changes", o.changes);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FillContext, spot, spot_source, iv, delta, years, equity, floor_room, buying_power)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Excursion, pnl, time, spot)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TradeReview, worst, best, planned_risk, finished)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DayNote, plan, review, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Fill, id, order_id, symbol, side, quantity, price, fee, observation, quote_time, time, context, actor)
inline void from_json(const Json& j, Fill& f) {
  j.at("id").get_to(f.id); j.at("order_id").get_to(f.order_id); j.at("symbol").get_to(f.symbol);
  j.at("side").get_to(f.side); j.at("quantity").get_to(f.quantity); j.at("price").get_to(f.price);
  j.at("fee").get_to(f.fee); j.at("observation").get_to(f.observation);
  j.at("quote_time").get_to(f.quote_time); j.at("time").get_to(f.time);
  added_field(j, "context", f.context); added_field(j, "actor", f.actor);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(QuoteObservation, symbol, observation, time, bid, ask, bid_size, ask_size)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Valuation, symbol, time, delta, gamma, vega, theta, spot, forward, discount, years, smile_iv, valid, spot_source)
inline void from_json(const Json& j, Valuation& v) {
  j.at("symbol").get_to(v.symbol); j.at("time").get_to(v.time); j.at("delta").get_to(v.delta);
  j.at("gamma").get_to(v.gamma); j.at("vega").get_to(v.vega); j.at("theta").get_to(v.theta);
  j.at("spot").get_to(v.spot); j.at("forward").get_to(v.forward); j.at("discount").get_to(v.discount);
  j.at("years").get_to(v.years); j.at("smile_iv").get_to(v.smile_iv); j.at("valid").get_to(v.valid);
  added_field(j, "spot_source", v.spot_source);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Exposure, dollar_delta, dollar_gamma_1pct, vega, theta)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExposureLimits, dollar_delta, vega)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Limits, max_order_contracts, price_band_absolute, price_band_relative, aggregate, per_underlying, underlying_overrides, max_daily_loss, max_quote_age, max_valuation_age)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Guardrails, soft_floor, soft_floor_percent, max_opening_trades, cooldown_loss, cooldown_minutes, profit_lock)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(GuardrailState, opening_trades, cooldown_until, latched, owns_kill)
inline void from_json(const Json& j, GuardrailState& g) {
  added_field(j, "opening_trades", g.opening_trades); added_field(j, "cooldown_until", g.cooldown_until);
  added_field(j, "latched", g.latched); added_field(j, "owns_kill", g.owns_kill);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScenarioConfig, spot_percent, vol_points, vol_floor)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PayoutRules, qualifying_profit, qualifying_days, withdrawal_percent, split_percent, minimum, caps)
inline void to_json(Json& j, MarginMode mode) { j = mode == MarginMode::Portfolio ? "portfolio" : "strategy"; }
inline void from_json(const Json& j, MarginMode& mode) {
  const auto value = j.get<std::string>();
  if (value != "strategy" && value != "portfolio") throw TradingError(Reason::JOURNAL_CORRUPT, "Unknown recorded margin mode");
  mode = value == "portfolio" ? MarginMode::Portfolio : MarginMode::Strategy;
}
inline void to_json(Json& j, const AccountRules& r) {
  j = Json{{"plan", r.plan}, {"profit_target", r.profit_target}, {"max_drawdown", r.max_drawdown},
           {"drawdown_mode", r.drawdown_mode}, {"buy_only", r.buy_only}, {"buying_power", r.buying_power},
           {"expiry_cutoff", r.expiry_cutoff}, {"phase", r.phase}, {"lock_balance", r.lock_balance},
           {"payouts", r.payouts}, {"defined_risk", r.defined_risk}, {"slippage_ticks", r.slippage_ticks}, {"margin", r.margin}};
  // Preserve existing journal bytes when the optional models are off.
  if (r.fill_latency_ms != 0) j["fill_latency_ms"] = r.fill_latency_ms;
  if (r.impact_ticks != 0) j["impact_ticks"] = r.impact_ticks;
}
inline void from_json(const Json& j, AccountRules& r) {
  j.at("plan").get_to(r.plan); j.at("profit_target").get_to(r.profit_target); j.at("max_drawdown").get_to(r.max_drawdown);
  j.at("drawdown_mode").get_to(r.drawdown_mode); j.at("buy_only").get_to(r.buy_only); j.at("buying_power").get_to(r.buying_power);
  j.at("expiry_cutoff").get_to(r.expiry_cutoff);
  added_field(j, "phase", r.phase); added_field(j, "lock_balance", r.lock_balance); added_field(j, "payouts", r.payouts);
  added_field(j, "defined_risk", r.defined_risk);
  if (const auto it = j.find("slippage_ticks"); it != j.end() && !it->is_number_integer())
    throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded slippage must be an integer");
  added_field(j, "slippage_ticks", r.slippage_ticks); added_field(j, "margin", r.margin);
  for (const auto* key : {"fill_latency_ms", "impact_ticks"})
    if (const auto it = j.find(key); it != j.end() && !it->is_number_integer())
      throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded fill settings must be integers");
  r.fill_latency_ms = j.value("fill_latency_ms", std::int64_t{0});
  r.impact_ticks = j.value("impact_ticks", std::int64_t{0});
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(SessionConfig, initial_cash, fee_per_contract, limits, scenarios, rules, guardrails)
inline void from_json(const Json& j, SessionConfig& c) {
  j.at("initial_cash").get_to(c.initial_cash); j.at("fee_per_contract").get_to(c.fee_per_contract);
  j.at("limits").get_to(c.limits); j.at("scenarios").get_to(c.scenarios);
  added_field(j, "rules", c.rules); added_field(j, "guardrails", c.guardrails);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Attribution, delta, gamma, vega, theta, other, costs)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(EvaluationDay, day, open_equity, close_equity, peak, floor, realised, qualifying, attribution, low_equity, high_equity, low_at, high_at)
inline void from_json(const Json& j, EvaluationDay& d) {
  j.at("day").get_to(d.day); j.at("open_equity").get_to(d.open_equity); j.at("close_equity").get_to(d.close_equity);
  j.at("peak").get_to(d.peak); j.at("floor").get_to(d.floor);
  added_field(j, "realised", d.realised); added_field(j, "qualifying", d.qualifying);
  added_field(j, "attribution", d.attribution);
  added_field(j, "low_equity", d.low_equity); added_field(j, "high_equity", d.high_equity);
  added_field(j, "low_at", d.low_at); added_field(j, "high_at", d.high_at);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Payout, number, time, day, amount, trader_share, balance)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Evaluation, attempt, started, starting_balance, peak, floor, status, decided_at, decided_equity, decision, first_order, first_fill, day, day_open_equity, day_close_equity, days, floor_locked, day_open_realised, qualifying_days, cycle_started, payouts, day_low_equity, day_high_equity, day_low_at, day_high_at, closest_floor, closest_floor_at)
inline void from_json(const Json& j, Evaluation& e) {
  j.at("attempt").get_to(e.attempt); j.at("started").get_to(e.started); j.at("starting_balance").get_to(e.starting_balance);
  j.at("peak").get_to(e.peak); j.at("floor").get_to(e.floor); j.at("status").get_to(e.status);
  j.at("decided_at").get_to(e.decided_at); j.at("decided_equity").get_to(e.decided_equity); j.at("decision").get_to(e.decision);
  j.at("first_order").get_to(e.first_order); j.at("first_fill").get_to(e.first_fill); j.at("day").get_to(e.day);
  j.at("day_open_equity").get_to(e.day_open_equity); j.at("day_close_equity").get_to(e.day_close_equity); j.at("days").get_to(e.days);
  added_field(j, "floor_locked", e.floor_locked); added_field(j, "day_open_realised", e.day_open_realised);
  added_field(j, "qualifying_days", e.qualifying_days); added_field(j, "cycle_started", e.cycle_started);
  added_field(j, "payouts", e.payouts);
  added_field(j, "day_low_equity", e.day_low_equity); added_field(j, "day_high_equity", e.day_high_equity);
  added_field(j, "day_low_at", e.day_low_at); added_field(j, "day_high_at", e.day_high_at);
  added_field(j, "closest_floor", e.closest_floor); added_field(j, "closest_floor_at", e.closest_floor_at);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AttemptSummary, attempt, plan, started, ended, starting_balance, final_equity, status, decision, first_order, first_fill)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Closure, symbol, quantity, price, time, kind, after_fill)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StockFill, id, symbol, shares, price, time, source, option)
inline void to_json(Json& j, const DividendPayment& d) {
  j = Json{{"symbol", d.symbol}, {"ex_date", d.ex_date}, {"per_share", d.per_share}, {"shares", d.shares},
           {"amount", d.amount}, {"time", d.time}};
  if (d.after_stock_fill) j["after_stock_fill"] = *d.after_stock_fill;
}
inline void from_json(const Json& j, DividendPayment& d) {
  j.at("symbol").get_to(d.symbol); j.at("ex_date").get_to(d.ex_date); j.at("per_share").get_to(d.per_share);
  j.at("shares").get_to(d.shares); j.at("amount").get_to(d.amount); j.at("time").get_to(d.time);
  added_field(j, "after_stock_fill", d.after_stock_fill);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ClosingPrint, price, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Annotation, note, tags, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BuyingPower, available, reserved, short_requirement)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Position, contract, quantity, basis, realised, fees)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Account, cash, realised, fees)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StockPosition, symbol, shares, basis, realised, fees)
inline void to_json(Json& j, const Ledger& l) {
  j = Json{{"account", l.account()}, {"positions", l.positions()}, {"stocks", l.stocks()}};
}
inline void from_json(const Json& j, Ledger& l) {
  std::map<std::string, StockPosition> stocks;
  added_field(j, "stocks", stocks);
  l = Ledger::restore(j.at("account").get<Account>(), j.at("positions").get<std::map<std::string, Position>>(), std::move(stocks));
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExposureRange, delta_low, delta_high, vega_low, vega_high)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RiskBucket, position, reachable, limits, delta_utilisation, vega_utilisation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RiskSnapshot, aggregate, underlyings, complete, daily_loss, kill_latched, kill_reason, limits_revision)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScenarioCell, spot_percent, vol_points, pnl, clamped)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScenarioGrid, cells, complete)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MarkedPosition, position, mark, mark_time, mark_age, market_value, unrealised, fresh, awaiting_settlement)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MarkedStock, position, mark, mark_time, market_value, unrealised, fresh)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(TradingSnapshot, account_version, time, account, equity, start_of_day_equity, unrealised, valuation_complete, journal_failed, positions, stocks, open_orders, recent_orders, recent_fills, risk, scenarios, quality_flags, evaluation, buying_power, closures, attempts, annotations, attribution, attributions, stock_fills, dividends, closing_prints, day_notes, trade_reviews, strategy_reviews, pending_limits, pending_guardrails, guardrails, pending_applied_at, soft_floor)
inline void from_json(const Json& j, TradingSnapshot& s) {
  j.at("account_version").get_to(s.account_version); j.at("time").get_to(s.time); j.at("account").get_to(s.account);
  j.at("equity").get_to(s.equity); j.at("start_of_day_equity").get_to(s.start_of_day_equity);
  j.at("unrealised").get_to(s.unrealised); j.at("valuation_complete").get_to(s.valuation_complete);
  j.at("journal_failed").get_to(s.journal_failed); j.at("positions").get_to(s.positions);
  j.at("open_orders").get_to(s.open_orders); j.at("recent_orders").get_to(s.recent_orders);
  j.at("recent_fills").get_to(s.recent_fills); j.at("risk").get_to(s.risk); j.at("scenarios").get_to(s.scenarios);
  j.at("quality_flags").get_to(s.quality_flags);
  added_field(j, "evaluation", s.evaluation); added_field(j, "buying_power", s.buying_power);
  added_field(j, "closures", s.closures); added_field(j, "attempts", s.attempts);
  added_field(j, "annotations", s.annotations);
  added_field(j, "attribution", s.attribution); added_field(j, "attributions", s.attributions);
  added_field(j, "stocks", s.stocks); added_field(j, "stock_fills", s.stock_fills);
  added_field(j, "dividends", s.dividends); added_field(j, "closing_prints", s.closing_prints);
  added_field(j, "day_notes", s.day_notes); added_field(j, "trade_reviews", s.trade_reviews);
  added_field(j, "strategy_reviews", s.strategy_reviews);
  added_field(j, "pending_limits", s.pending_limits); added_field(j, "pending_guardrails", s.pending_guardrails);
  added_field(j, "guardrails", s.guardrails); added_field(j, "pending_applied_at", s.pending_applied_at);
  added_field(j, "soft_floor", s.soft_floor);
}

namespace detail {
struct Book {
  QuoteObservation quote;
  // With impact enabled, negative budgets count contracts beyond displayed size.
  Quantity bid_left = 0;
  Quantity ask_left = 0;
};
struct Mark {
  Money price;
  Timestamp time = 0;
};
/// Where a held position's current stretch at one size began: its size, mark
/// and, when valid, valuation. Today's P&L by Greek runs from here.
struct Reference {
  Quantity quantity = 0;
  Money mark;
  std::optional<Valuation> valuation;
};
/// Derived, never journaled: the lifecycles whose reviews can still change, kept
/// up to date as fills and closures are recorded (`fills` and `closures` of them
/// applied so far), and how many reviews are unfinished. update_reviews then costs
/// the open positions rather than every fill. Built on first use after a load.
struct Reviewing {
  bool ready = false;
  std::size_t fills = 0;
  std::size_t closures = 0;
  LifecycleBuilder builder;
  std::size_t unfinished = 0;
};
struct State {
  std::string actor = "system"; ///< Transient command context, not persisted as account state.
  SessionConfig config;
  Timestamp time = 0;
  std::uint64_t version = 0;
  std::uint64_t limits_revision = 1;
  Ledger ledger;
  Money start_equity;
  md::Date day;
  Contracts contracts;
  SharedMap<std::string, Book> books;
  SharedMap<std::string, Mark> marks;
  Valuations valuations;
  SharedVector<Order> orders;
  SharedVector<Fill> fills;
  SharedSet<std::string> settled;
  bool kill = false;
  std::string kill_reason;
  Evaluation evaluation;
  std::optional<Limits> pending_limits;
  std::optional<Guardrails> pending_guardrails;
  GuardrailState guardrails;
  Timestamp pending_applied_at = 0;
  std::vector<AttemptSummary> attempts;
  SharedVector<Closure> closures;
  SharedMap<std::string, Annotation> annotations;
  SharedMap<std::string, DayNote> day_notes;
  SharedMap<std::string, TradeReview> trade_reviews;
  SharedMap<std::string, TradeReview> strategy_reviews;
  /// Open stretches by held contract (or stock), and today's finished ones (and costs).
  std::map<std::string, Reference> references;
  std::map<std::string, Attribution> explained;
  /// The underlyings' latest prices, which mark and trade delivered shares.
  std::map<std::string, Mark> stock_marks;
  SharedVector<StockFill> stock_fills;
  SharedVector<DividendPayment> dividends;
  SharedMap<std::string, ClosingPrint> closing_prints;
  /// Derived, never journaled: the IDs of the orders open when the orders were
  /// last indexed, in ID order, and how many orders there were then. A closed
  /// order never reopens, so those still open and the orders placed since are
  /// every open order, and a scan for them costs the open orders, not the past.
  std::vector<OrderId> working;
  std::size_t indexed = 0;
  /// Derived, never journaled: the first order each client order ID named.
  SharedMap<std::string, OrderId> clients;
  /// Derived, never journaled: see Reviewing.
  Reviewing reviewing;
  /// Transient, never journaled: the settlement reference of each contract settled
  /// by the transaction in progress, the underlying's price for the last review
  /// sample of the round trip it closed. update_reviews consumes and clears it.
  std::map<std::string, Money> settling;
};
/// The open orders' IDs, in ID order.
inline std::vector<OrderId> open_ids(const State& s) {
  std::vector<OrderId> ids;
  for (const auto id : s.working)
    if (id <= s.orders.size() && s.orders[id - 1].open()) ids.push_back(id);
  for (auto i = s.indexed; i < s.orders.size(); ++i)
    if (s.orders[i].open()) ids.push_back(static_cast<OrderId>(i + 1));
  return ids;
}
inline void reindex(State& s) {
  s.working = open_ids(s);
  s.indexed = s.orders.size();
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Book, quote, bid_left, ask_left)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Mark, price, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Reference, quantity, mark, valuation)
/// Every journaled field of State, once: its JSON writer and the journal's
/// change finder (state_change) both come from this list.
#define OPENPORT_STATE_FIELDS(X) \
  X(config) X(time) X(version) X(limits_revision) X(ledger) X(start_equity) X(day) X(contracts) X(books) \
  X(marks) X(valuations) X(orders) X(fills) X(settled) X(kill) X(kill_reason) X(evaluation) X(attempts) \
  X(closures) X(annotations) X(references) X(explained) X(stock_marks) X(stock_fills) X(dividends) \
  X(closing_prints) X(day_notes) X(trade_reviews) X(strategy_reviews) X(pending_limits) \
  X(pending_guardrails) X(guardrails) X(pending_applied_at)
inline void to_json(Json& j, const State& s) {
#define OPENPORT_STATE_TO(field) j[#field] = s.field;
  OPENPORT_STATE_FIELDS(OPENPORT_STATE_TO)
#undef OPENPORT_STATE_TO
}
inline void from_json(const Json& j, State& s) {
  j.at("config").get_to(s.config); j.at("time").get_to(s.time); j.at("version").get_to(s.version);
  j.at("limits_revision").get_to(s.limits_revision); j.at("ledger").get_to(s.ledger);
  j.at("start_equity").get_to(s.start_equity); j.at("day").get_to(s.day); j.at("contracts").get_to(s.contracts);
  j.at("books").get_to(s.books); j.at("marks").get_to(s.marks); j.at("valuations").get_to(s.valuations);
  j.at("orders").get_to(s.orders); j.at("fills").get_to(s.fills); j.at("settled").get_to(s.settled);
  j.at("kill").get_to(s.kill); j.at("kill_reason").get_to(s.kill_reason);
  added_field(j, "evaluation", s.evaluation); added_field(j, "attempts", s.attempts);
  added_field(j, "closures", s.closures); added_field(j, "annotations", s.annotations);
  added_field(j, "references", s.references); added_field(j, "explained", s.explained);
  added_field(j, "stock_marks", s.stock_marks); added_field(j, "stock_fills", s.stock_fills);
  added_field(j, "dividends", s.dividends); added_field(j, "closing_prints", s.closing_prints);
  added_field(j, "day_notes", s.day_notes); added_field(j, "trade_reviews", s.trade_reviews);
  added_field(j, "strategy_reviews", s.strategy_reviews);
  added_field(j, "pending_limits", s.pending_limits); added_field(j, "pending_guardrails", s.pending_guardrails);
  added_field(j, "guardrails", s.guardrails); added_field(j, "pending_applied_at", s.pending_applied_at);
  s.working.clear();
  s.indexed = 0;
  reindex(s);
  s.clients.clear();
  for (const auto& order : s.orders) s.clients.emplace(order.request.client_order_id, order.id);
  s.reviewing = {};
  s.settling.clear();
}
}  // namespace detail
}  // namespace openport::trading

#pragma once

#include <set>
#include <nlohmann/json.hpp>
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
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(OrderRequest, client_order_id, symbol, side, type, tif, quantity, limit_price, trigger, bracket, legs)
inline void from_json(const Json& j, OrderRequest& r) {
  j.at("client_order_id").get_to(r.client_order_id); j.at("symbol").get_to(r.symbol); j.at("side").get_to(r.side);
  j.at("type").get_to(r.type); j.at("tif").get_to(r.tif); j.at("quantity").get_to(r.quantity);
  j.at("limit_price").get_to(r.limit_price);
  added_field(j, "trigger", r.trigger); added_field(j, "bracket", r.bracket); added_field(j, "legs", r.legs);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Order, id, request, status, filled_quantity, filled_notional, accepted_at, day_end, reason, system, role, parent, oco, stop_loss, take_profit, triggered_at)
inline void from_json(const Json& j, Order& o) {
  j.at("id").get_to(o.id); j.at("request").get_to(o.request); j.at("status").get_to(o.status);
  j.at("filled_quantity").get_to(o.filled_quantity); j.at("filled_notional").get_to(o.filled_notional);
  j.at("accepted_at").get_to(o.accepted_at); j.at("day_end").get_to(o.day_end); j.at("reason").get_to(o.reason);
  added_field(j, "system", o.system); added_field(j, "role", o.role); added_field(j, "parent", o.parent);
  added_field(j, "oco", o.oco); added_field(j, "stop_loss", o.stop_loss); added_field(j, "take_profit", o.take_profit);
  added_field(j, "triggered_at", o.triggered_at);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Fill, id, order_id, symbol, side, quantity, price, fee, observation, quote_time, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(QuoteObservation, symbol, observation, time, bid, ask, bid_size, ask_size)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Valuation, symbol, time, delta, gamma, vega, theta, spot, forward, discount, years, smile_iv, valid)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Exposure, dollar_delta, dollar_gamma_1pct, vega, theta)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExposureLimits, dollar_delta, vega)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Limits, max_order_contracts, price_band_absolute, price_band_relative, aggregate, per_underlying, underlying_overrides, max_daily_loss, max_quote_age, max_valuation_age)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScenarioConfig, spot_percent, vol_points, vol_floor)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PayoutRules, qualifying_profit, qualifying_days, withdrawal_percent, split_percent, minimum, caps)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(AccountRules, plan, profit_target, max_drawdown, drawdown_mode, buy_only, buying_power, expiry_cutoff, phase, lock_balance, payouts)
inline void from_json(const Json& j, AccountRules& r) {
  j.at("plan").get_to(r.plan); j.at("profit_target").get_to(r.profit_target); j.at("max_drawdown").get_to(r.max_drawdown);
  j.at("drawdown_mode").get_to(r.drawdown_mode); j.at("buy_only").get_to(r.buy_only); j.at("buying_power").get_to(r.buying_power);
  j.at("expiry_cutoff").get_to(r.expiry_cutoff);
  added_field(j, "phase", r.phase); added_field(j, "lock_balance", r.lock_balance); added_field(j, "payouts", r.payouts);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(SessionConfig, initial_cash, fee_per_contract, limits, scenarios, rules)
inline void from_json(const Json& j, SessionConfig& c) {
  j.at("initial_cash").get_to(c.initial_cash); j.at("fee_per_contract").get_to(c.fee_per_contract);
  j.at("limits").get_to(c.limits); j.at("scenarios").get_to(c.scenarios);
  added_field(j, "rules", c.rules);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Attribution, delta, gamma, vega, theta, other, costs)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(EvaluationDay, day, open_equity, close_equity, peak, floor, realised, qualifying, attribution)
inline void from_json(const Json& j, EvaluationDay& d) {
  j.at("day").get_to(d.day); j.at("open_equity").get_to(d.open_equity); j.at("close_equity").get_to(d.close_equity);
  j.at("peak").get_to(d.peak); j.at("floor").get_to(d.floor);
  added_field(j, "realised", d.realised); added_field(j, "qualifying", d.qualifying);
  added_field(j, "attribution", d.attribution);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Payout, number, time, day, amount, trader_share, balance)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Evaluation, attempt, started, starting_balance, peak, floor, status, decided_at, decided_equity, decision, first_order, first_fill, day, day_open_equity, day_close_equity, days, floor_locked, day_open_realised, qualifying_days, cycle_started, payouts)
inline void from_json(const Json& j, Evaluation& e) {
  j.at("attempt").get_to(e.attempt); j.at("started").get_to(e.started); j.at("starting_balance").get_to(e.starting_balance);
  j.at("peak").get_to(e.peak); j.at("floor").get_to(e.floor); j.at("status").get_to(e.status);
  j.at("decided_at").get_to(e.decided_at); j.at("decided_equity").get_to(e.decided_equity); j.at("decision").get_to(e.decision);
  j.at("first_order").get_to(e.first_order); j.at("first_fill").get_to(e.first_fill); j.at("day").get_to(e.day);
  j.at("day_open_equity").get_to(e.day_open_equity); j.at("day_close_equity").get_to(e.day_close_equity); j.at("days").get_to(e.days);
  added_field(j, "floor_locked", e.floor_locked); added_field(j, "day_open_realised", e.day_open_realised);
  added_field(j, "qualifying_days", e.qualifying_days); added_field(j, "cycle_started", e.cycle_started);
  added_field(j, "payouts", e.payouts);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AttemptSummary, attempt, plan, started, ended, starting_balance, final_equity, status, decision, first_order, first_fill)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Closure, symbol, quantity, price, time, kind, after_fill)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StockFill, id, symbol, shares, price, time, source, option)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DividendPayment, symbol, ex_date, per_share, shares, amount, time)
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
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(TradingSnapshot, account_version, time, account, equity, start_of_day_equity, unrealised, valuation_complete, journal_failed, positions, stocks, open_orders, recent_orders, recent_fills, risk, scenarios, quality_flags, evaluation, buying_power, closures, attempts, annotations, attribution, attributions, stock_fills, dividends, closing_prints)
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
}

namespace detail {
struct Book {
  QuoteObservation quote;
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
struct State {
  SessionConfig config;
  Timestamp time = 0;
  std::uint64_t version = 0;
  std::uint64_t limits_revision = 1;
  Ledger ledger;
  Money start_equity;
  md::Date day;
  std::map<std::string, md::OptionContract> contracts;
  std::map<std::string, Book> books;
  std::map<std::string, Mark> marks;
  std::map<std::string, Valuation> valuations;
  std::vector<Order> orders;
  std::vector<Fill> fills;
  std::set<std::string> settled;
  bool kill = false;
  std::string kill_reason;
  Evaluation evaluation;
  std::vector<AttemptSummary> attempts;
  std::vector<Closure> closures;
  std::map<std::string, Annotation> annotations;
  /// Open stretches by held contract (or stock), and today's finished ones (and costs).
  std::map<std::string, Reference> references;
  std::map<std::string, Attribution> explained;
  /// The underlyings' latest prices, which mark and trade delivered shares.
  std::map<std::string, Mark> stock_marks;
  std::vector<StockFill> stock_fills;
  std::vector<DividendPayment> dividends;
  std::map<std::string, ClosingPrint> closing_prints;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Book, quote, bid_left, ask_left)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Mark, price, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Reference, quantity, mark, valuation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(State, config, time, version, limits_revision, ledger, start_equity, day, contracts, books, marks, valuations, orders, fills, settled, kill, kill_reason, evaluation, attempts, closures, annotations, references, explained, stock_marks, stock_fills, dividends, closing_prints)
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
}
}  // namespace detail
}  // namespace openport::trading

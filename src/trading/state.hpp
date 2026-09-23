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
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OrderRequest, client_order_id, symbol, side, type, tif, quantity, limit_price)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Order, id, request, status, filled_quantity, filled_notional, accepted_at, day_end, reason, system)
inline void from_json(const Json& j, Order& o) {
  j.at("id").get_to(o.id); j.at("request").get_to(o.request); j.at("status").get_to(o.status);
  j.at("filled_quantity").get_to(o.filled_quantity); j.at("filled_notional").get_to(o.filled_notional);
  j.at("accepted_at").get_to(o.accepted_at); j.at("day_end").get_to(o.day_end); j.at("reason").get_to(o.reason);
  added_field(j, "system", o.system);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Fill, id, order_id, symbol, side, quantity, price, fee, observation, quote_time, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(QuoteObservation, symbol, observation, time, bid, ask, bid_size, ask_size)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Valuation, symbol, time, delta, gamma, vega, theta, spot, forward, discount, years, smile_iv, valid)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Exposure, dollar_delta, dollar_gamma_1pct, vega, theta)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExposureLimits, dollar_delta, vega)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Limits, max_order_contracts, price_band_absolute, price_band_relative, aggregate, per_underlying, underlying_overrides, max_daily_loss, max_quote_age, max_valuation_age)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScenarioConfig, spot_percent, vol_points, vol_floor)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AccountRules, plan, profit_target, max_drawdown, drawdown_mode, buy_only, buying_power, expiry_cutoff)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(SessionConfig, initial_cash, fee_per_contract, limits, scenarios, rules)
inline void from_json(const Json& j, SessionConfig& c) {
  j.at("initial_cash").get_to(c.initial_cash); j.at("fee_per_contract").get_to(c.fee_per_contract);
  j.at("limits").get_to(c.limits); j.at("scenarios").get_to(c.scenarios);
  added_field(j, "rules", c.rules);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EvaluationDay, day, open_equity, close_equity, peak, floor)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Evaluation, attempt, started, starting_balance, peak, floor, status, decided_at, decided_equity, decision, first_order, first_fill, day, day_open_equity, day_close_equity, days)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AttemptSummary, attempt, plan, started, ended, starting_balance, final_equity, status, decision, first_order, first_fill)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Closure, symbol, quantity, price, time, kind, after_fill)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BuyingPower, available, reserved, short_requirement)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Position, contract, quantity, basis, realised, fees)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Account, cash, realised, fees)
inline void to_json(Json& j, const Ledger& l) { j = Json{{"account", l.account()}, {"positions", l.positions()}}; }
inline void from_json(const Json& j, Ledger& l) {
  l = Ledger::restore(j.at("account").get<Account>(), j.at("positions").get<std::map<std::string, Position>>());
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExposureRange, delta_low, delta_high, vega_low, vega_high)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RiskBucket, position, reachable, limits, delta_utilisation, vega_utilisation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RiskSnapshot, aggregate, underlyings, complete, daily_loss, kill_latched, kill_reason, limits_revision)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScenarioCell, spot_percent, vol_points, pnl, clamped)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScenarioGrid, cells, complete)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MarkedPosition, position, mark, mark_time, mark_age, market_value, unrealised, fresh, awaiting_settlement)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(TradingSnapshot, account_version, time, account, equity, start_of_day_equity, unrealised, valuation_complete, journal_failed, positions, open_orders, recent_orders, recent_fills, risk, scenarios, quality_flags, evaluation, buying_power, closures, attempts)
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
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Book, quote, bid_left, ask_left)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Mark, price, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(State, config, time, version, limits_revision, ledger, start_equity, day, contracts, books, marks, valuations, orders, fills, settled, kill, kill_reason, evaluation, attempts, closures)
inline void from_json(const Json& j, State& s) {
  j.at("config").get_to(s.config); j.at("time").get_to(s.time); j.at("version").get_to(s.version);
  j.at("limits_revision").get_to(s.limits_revision); j.at("ledger").get_to(s.ledger);
  j.at("start_equity").get_to(s.start_equity); j.at("day").get_to(s.day); j.at("contracts").get_to(s.contracts);
  j.at("books").get_to(s.books); j.at("marks").get_to(s.marks); j.at("valuations").get_to(s.valuations);
  j.at("orders").get_to(s.orders); j.at("fills").get_to(s.fills); j.at("settled").get_to(s.settled);
  j.at("kill").get_to(s.kill); j.at("kill_reason").get_to(s.kill_reason);
  added_field(j, "evaluation", s.evaluation); added_field(j, "attempts", s.attempts);
  added_field(j, "closures", s.closures);
}
}  // namespace detail
}  // namespace openport::trading

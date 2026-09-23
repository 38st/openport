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
  for (int i = 0; i <= static_cast<int>(Reason::JOURNAL_LOCKED); ++i) {
    const auto candidate = static_cast<Reason>(i);
    if (to_string(candidate) == text) { r = candidate; return; }
  }
  throw TradingError(Reason::JOURNAL_CORRUPT, "Unknown recorded reason code");
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Decision, code, message, actual, limit, scope)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OrderRequest, client_order_id, symbol, side, type, tif, quantity, limit_price)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Order, id, request, status, filled_quantity, filled_notional, accepted_at, day_end, reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Fill, id, order_id, symbol, side, quantity, price, fee, observation, quote_time, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(QuoteObservation, symbol, observation, time, bid, ask, bid_size, ask_size)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Valuation, symbol, time, delta, gamma, vega, theta, spot, forward, discount, years, smile_iv, valid)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Exposure, dollar_delta, dollar_gamma_1pct, vega, theta)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExposureLimits, dollar_delta, vega)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Limits, max_order_contracts, price_band_absolute, price_band_relative, aggregate, per_underlying, underlying_overrides, max_daily_loss, max_quote_age, max_valuation_age)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScenarioConfig, spot_percent, vol_points, vol_floor)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionConfig, initial_cash, fee_per_contract, limits, scenarios)
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
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TradingSnapshot, account_version, time, account, equity, start_of_day_equity, unrealised, valuation_complete, journal_failed, positions, open_orders, recent_orders, recent_fills, risk, scenarios, quality_flags)

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
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Book, quote, bid_left, ask_left)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Mark, price, time)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(State, config, time, version, limits_revision, ledger, start_equity, day, contracts, books, marks, valuations, orders, fills, settled, kill, kill_reason)
}  // namespace detail
}  // namespace openport::trading

#pragma once

#include "openport/trading/ledger.hpp"

namespace openport::trading {

struct ExposureRange {
  double delta_low = 0;
  double delta_high = 0;
  double vega_low = 0;
  double vega_high = 0;
};
struct RiskBucket {
  Exposure position;
  ExposureRange reachable;  ///< Any subset of open orders, independently per factor.
  ExposureLimits limits;
  double delta_utilisation = 0;
  double vega_utilisation = 0;
};
struct RiskSnapshot {
  RiskBucket aggregate;
  std::map<std::string, RiskBucket> underlyings;
  bool complete = true;
  Money daily_loss;  ///< max(0, start-of-day equity - equity).
  bool kill_latched = false;
  std::string kill_reason;
  std::uint64_t limits_revision = 1;
};

/// The market time a contract's quotes and valuations must be recent to: `now`
/// while one of its sessions is open, otherwise the end of its last session, so
/// a closed market's close stays current until it reopens (an SPY position
/// overnight, while SPX trades).
[[nodiscard]] Timestamp observation_time(const md::OptionContract& contract, Timestamp now);

/// Shares count at their dollar delta, at `stock_prices` (fresh prices by
/// underlying); a holding without one leaves the result incomplete. `orders` are
/// the pending orders, by pointer so that a book of many need not be copied.
[[nodiscard]] RiskSnapshot portfolio_risk(
    const Ledger& ledger, const std::vector<const Order*>& orders, const Contracts& contracts,
    const Valuations& valuations, const Limits& limits, Timestamp now,
    const std::map<std::string, double>& stock_prices = {});
[[nodiscard]] Decision check_exposure(const RiskSnapshot& risk);
/// One order's check: `risk` with the order among the pending ones, `without`
/// the same book without it. A bucket over its limit refuses the order only when
/// the order raises that bucket's worst reachable exposure, so a book already
/// over a limit can still close, hedge and trade other underlyings.
[[nodiscard]] Decision check_exposure(const RiskSnapshot& risk, const RiskSnapshot& without);

struct ScenarioCell {
  double spot_percent = 0;
  double vol_points = 0;
  double pnl = 0;  ///< Analytical dollars, not a booked cash amount.
  bool clamped = false;
};
struct ScenarioGrid {
  std::vector<ScenarioCell> cells;  ///< Spot-major, vol-minor.
  bool complete = true;
};
void validate_scenarios(const ScenarioConfig& config);
[[nodiscard]] ScenarioGrid scenario_grid(const Ledger& ledger,
    const Valuations& valuations, const ScenarioConfig& config,
    Timestamp now, Timestamp max_age, const std::map<std::string, double>& stock_prices = {});

struct BreachLevel {
  double points = 0;
  double percent = 0;
  std::optional<double> touch_probability;
};
struct UnderlyingBreach {
  std::string underlying;
  double spot = 0;
  /// One standard deviation of the log price to today's regular close, from the
  /// market's implied variance (see Engine's close_variance).
  std::optional<double> close_sigma;
  std::optional<BreachLevel> down;
  std::optional<BreachLevel> up;
  /// Where equity would reach the personal soft floor, which liquidates first
  /// when it is nearer than the plan floor; the same as down/up without a plan floor.
  std::optional<BreachLevel> soft_down;
  std::optional<BreachLevel> soft_up;
  bool complete = false;
};
struct BreachRisk {
  std::optional<Money> room;
  std::optional<Money> soft_room;
  bool complete = true;
  std::vector<UnderlyingBreach> underlyings;
};
/// Something about the held book worth acting on before the market or the night
/// acts on it. `actual` and `limit` are numbers whose meaning depends on the code
/// (see TradingSession::warnings).
struct RiskWarning {
  std::string code;
  std::string severity;  ///< "warning": it costs room, buying power or a fill soon; "info": worth knowing.
  std::string scope;     ///< The underlying, or "aggregate" for the account.
  std::string symbol;    ///< The contract it is about, if one; empty otherwise.
  std::string message;
  std::optional<double> actual;
  std::optional<double> limit;
};

/// Every held underlying, with levels where equity reaches the plan floor, or the
/// soft floor on an account without one, and separately the soft floor's levels;
/// with neither floor there are no levels.
/// One underlying moves at a time, today's vol and remaining option life held
/// fixed. Search to -99.75% / +1000%, then bisect the first crossing. Missing
/// ATM vol leaves the driftless log-return reflection estimate absent.
[[nodiscard]] BreachRisk breach_risk(const Ledger& ledger,
    const Valuations& valuations, Money equity,
    std::optional<Money> floor, std::optional<Money> soft_floor, Timestamp now, Timestamp max_age,
    const std::map<std::string, double>& stock_prices = {}, const std::map<std::string, double>& close_variances = {});

}  // namespace openport::trading

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
/// underlying); a holding without one leaves the result incomplete.
[[nodiscard]] RiskSnapshot portfolio_risk(
    const Ledger& ledger, const std::vector<Order>& orders,
    const std::map<std::string, md::OptionContract>& contracts,
    const std::map<std::string, Valuation>& valuations, const Limits& limits, Timestamp now,
    const std::map<std::string, double>& stock_prices = {});
[[nodiscard]] Decision check_exposure(const RiskSnapshot& risk);

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
    const std::map<std::string, Valuation>& valuations, const ScenarioConfig& config,
    Timestamp now, Timestamp max_age, const std::map<std::string, double>& stock_prices = {});

}  // namespace openport::trading

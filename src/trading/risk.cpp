#include "openport/trading/risk.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "openport/pricing/black.hpp"

namespace openport::trading {
namespace {
bool fresh(const Valuation& v, Timestamp now, Timestamp max_age) {
  return valid_valuation(v) && v.time >= 0 && v.time <= now && now - v.time <= max_age;
}
Exposure exposure(Quantity q, const Valuation& v) {
  const double units = static_cast<double>(q) * 100;
  return {units * v.delta * v.spot, units * v.gamma * v.spot * v.spot * 0.01,
          units * v.vega, units * v.theta};
}
bool finite(const Exposure& e) {
  return std::isfinite(e.dollar_delta) && std::isfinite(e.dollar_gamma_1pct) &&
         std::isfinite(e.vega) && std::isfinite(e.theta);
}
bool add(RiskBucket& b, const Exposure& e, bool pending) {
  if (pending) {
    b.reachable.delta_low += std::min(0.0, e.dollar_delta);
    b.reachable.delta_high += std::max(0.0, e.dollar_delta);
    b.reachable.vega_low += std::min(0.0, e.vega);
    b.reachable.vega_high += std::max(0.0, e.vega);
  } else {
    b.position.dollar_delta += e.dollar_delta;
    b.position.dollar_gamma_1pct += e.dollar_gamma_1pct;
    b.position.vega += e.vega;
    b.position.theta += e.theta;
    b.reachable.delta_low += e.dollar_delta;
    b.reachable.delta_high += e.dollar_delta;
    b.reachable.vega_low += e.vega;
    b.reachable.vega_high += e.vega;
  }
  return finite(b.position) && std::isfinite(b.reachable.delta_low) &&
         std::isfinite(b.reachable.delta_high) && std::isfinite(b.reachable.vega_low) &&
         std::isfinite(b.reachable.vega_high);
}
double worst(double low, double high) { return std::max(std::abs(low), std::abs(high)); }
double utilisation(double value, double limit) {
  if (limit == 0) return value == 0 ? 0 : std::numeric_limits<double>::max();
  const double result = value / limit;
  return std::isfinite(result) ? result : std::numeric_limits<double>::max();
}
void finish(RiskBucket& b) {
  b.delta_utilisation = utilisation(worst(b.reachable.delta_low, b.reachable.delta_high), b.limits.dollar_delta);
  b.vega_utilisation = utilisation(worst(b.reachable.vega_low, b.reachable.vega_high), b.limits.vega);
}
Decision check_bucket(const RiskBucket& b, const std::string& scope) {
  const double delta = worst(b.reachable.delta_low, b.reachable.delta_high);
  const double vega = worst(b.reachable.vega_low, b.reachable.vega_high);
  if (delta > b.limits.dollar_delta)
    return {Reason::DELTA_LIMIT, "Worst reachable absolute dollar delta exceeds limit", delta, b.limits.dollar_delta, scope};
  if (vega > b.limits.vega)
    return {Reason::VEGA_LIMIT, "Worst reachable absolute vega per point exceeds limit", vega, b.limits.vega, scope};
  return {};
}
}  // namespace
RiskSnapshot portfolio_risk(const Ledger& ledger, const std::vector<Order>& orders,
    const std::map<std::string, md::OptionContract>& contracts,
    const std::map<std::string, Valuation>& valuations, const Limits& limits, Timestamp now) {
  RiskSnapshot result;
  result.aggregate.limits = limits.aggregate;
  auto accumulate = [&](const md::OptionContract& c, Quantity q, bool pending) {
    auto& bucket = result.underlyings[c.underlying];
    const auto override = limits.underlying_overrides.find(c.underlying);
    bucket.limits = override == limits.underlying_overrides.end() ? limits.per_underlying : override->second;
    const auto it = valuations.find(c.osi_symbol());
    if (now >= c.expiry_time() || it == valuations.end() || !fresh(it->second, now, limits.max_valuation_age)) {
      result.complete = false;
      return;
    }
    const auto e = exposure(q, it->second);
    if (!finite(e)) { result.complete = false; return; }
    auto next_bucket = bucket;
    auto next_aggregate = result.aggregate;
    if (!add(next_bucket, e, pending) || !add(next_aggregate, e, pending)) {
      result.complete = false;
      return;
    }
    bucket = next_bucket;
    result.aggregate = next_aggregate;
  };
  for (const auto& [symbol, p] : ledger.positions()) accumulate(p.contract, p.quantity, false);
  for (const auto& o : orders) {
    if (!o.open()) continue;
    const auto it = contracts.find(o.request.symbol);
    if (it == contracts.end()) { result.complete = false; continue; }
    accumulate(it->second, o.request.side == Side::Buy ? o.remaining() : -o.remaining(), true);
  }
  finish(result.aggregate);
  for (auto& [name, bucket] : result.underlyings) finish(bucket);
  return result;
}
Decision check_exposure(const RiskSnapshot& risk) {
  if (!risk.complete) return {Reason::MISSING_VALUATION, "Fresh complete portfolio and pending-order valuations required", {}, {}, {}};
  for (const auto& [name, bucket] : risk.underlyings) {
    auto result = check_bucket(bucket, name);
    if (!result.ok()) return result;
  }
  return check_bucket(risk.aggregate, "aggregate");
}
void validate_scenarios(const ScenarioConfig& c) {
  bool valid = !c.spot_percent.empty() && !c.vol_points.empty() &&
               c.spot_percent.size() <= 101 && c.vol_points.size() <= 101 &&
               std::isfinite(c.vol_floor) && c.vol_floor > 0 && c.vol_floor <= 10;
  for (double x : c.spot_percent) valid &= std::isfinite(x) && x > -100 && x <= 1000;
  for (double y : c.vol_points) valid &= std::isfinite(y) && std::abs(y) <= 1000;
  if (!valid) throw TradingError(Reason::INVALID_SCENARIO, "Invalid or oversized spot/vol grid");
}
ScenarioGrid scenario_grid(const Ledger& ledger, const std::map<std::string, Valuation>& valuations,
    const ScenarioConfig& config, Timestamp now, Timestamp max_age) {
  validate_scenarios(config);
  ScenarioGrid result;
  for (double spot : config.spot_percent) {
    for (double vol : config.vol_points) {
      ScenarioCell cell{spot, vol, 0, false};
      for (const auto& [symbol, p] : ledger.positions()) {
        const auto it = valuations.find(symbol);
        if (now >= p.contract.expiry_time() || it == valuations.end() || !fresh(it->second, now, max_age)) {
          result.complete = false;
          continue;
        }
        const auto& v = it->second;
        const double shocked_vol = v.smile_iv + vol / 100;
        cell.clamped |= shocked_vol < config.vol_floor;
        // Avoid subtraction and floor artifacts: zero shock is exactly zero.
        if (spot == 0 && vol == 0) continue;
        const double base = pricing::black_price(p.contract.type, v.forward, p.contract.strike, v.years, v.smile_iv, v.discount);
        const double shocked = pricing::black_price(p.contract.type, v.forward * (1 + spot / 100),
            p.contract.strike, v.years, std::max(config.vol_floor, shocked_vol), v.discount);
        const double pnl = static_cast<double>(p.quantity) * 100 * (shocked - base);
        if (!std::isfinite(pnl) || !std::isfinite(cell.pnl + pnl)) result.complete = false;
        else cell.pnl += pnl;
      }
      result.cells.push_back(cell);
    }
  }
  // An incomplete grid must not look like a valuation of only the covered subset.
  if (!result.complete) for (auto& cell : result.cells) cell.pnl = 0;
  return result;
}
}  // namespace openport::trading

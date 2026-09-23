#include <algorithm>
#include <cmath>
#include <map>

#include "openport/trading/evaluation.hpp"

namespace openport::trading {
namespace {
using pricing::OptionType;
/// Strikes are validated to whole thousandths of a dollar.
std::int64_t milli(double strike) { return std::llround(strike * 1000); }
/// One contract's intrinsic value at expiry, times the multiplier, at a spot in thousandths.
Money intrinsic(const md::OptionContract& c, std::int64_t spot) {
  const auto strike = milli(c.strike);
  const auto amount = std::max<std::int64_t>(0, c.type == OptionType::Call ? spot - strike : strike - spot);
  return Money::from_micros(amount * 1000) * 100;
}
/// Buy-back value plus the naked requirement for `n` of a short leg's contracts.
Money naked(const MarginLeg& leg, Quantity n) {
  return leg.value.prorate(n, -leg.quantity) + naked_requirement(leg.contract, leg.spot) * n;
}
Money verticals(const std::vector<const MarginLeg*>& group, OptionType type) {
  struct Side { std::int64_t strike; Quantity left; const MarginLeg* leg; };
  std::vector<Side> shorts, longs;
  for (const auto* leg : group) {
    if (leg->contract.type != type) continue;
    auto& list = leg->quantity < 0 ? shorts : longs;
    list.push_back({milli(leg->contract.strike), leg->quantity < 0 ? -leg->quantity : leg->quantity, leg});
  }
  // Puts pair from the highest strikes down, calls from the lowest up, so the
  // most exposed shorts meet the most protective longs first.
  const bool puts = type == OptionType::Put;
  const auto order = [puts](const Side& a, const Side& b) { return puts ? a.strike > b.strike : a.strike < b.strike; };
  std::stable_sort(shorts.begin(), shorts.end(), order);
  std::stable_sort(longs.begin(), longs.end(), order);
  Money cost;
  std::size_t next = 0;
  for (auto& s : shorts) {
    while (s.left > 0 && next < longs.size()) {
      auto& l = longs[next];
      const auto n = std::min(s.left, l.left);
      const auto width = std::max<std::int64_t>(0, puts ? s.strike - l.strike : l.strike - s.strike);
      cost = cost + std::min(Money::from_micros(width * 1000) * 100 * n, naked(*s.leg, n));
      s.left -= n;
      if ((l.left -= n) == 0) ++next;
    }
    if (s.left > 0) cost = cost + naked(*s.leg, s.left);
  }
  return cost;
}
/// The group's worst loss at expiry, or nothing when net short calls leave it unbounded.
std::optional<Money> worst_loss(const std::vector<const MarginLeg*>& group) {
  Quantity calls = 0;
  std::vector<std::int64_t> spots{0};
  for (const auto* leg : group) {
    if (leg->contract.type == OptionType::Call) calls += leg->quantity;
    spots.push_back(milli(leg->contract.strike));
  }
  if (calls < 0) return std::nullopt;
  Money worst;
  for (const auto spot : spots) {
    Money payoff;
    for (const auto* leg : group) payoff = payoff + intrinsic(leg->contract, spot) * leg->quantity;
    worst = std::min(worst, payoff);
  }
  return -worst;
}
}  // namespace

Money margin_requirement(const std::vector<MarginLeg>& legs) {
  std::map<std::pair<std::string, Timestamp>, std::vector<const MarginLeg*>> groups;
  for (const auto& leg : legs)
    if (leg.quantity != 0) groups[{leg.contract.underlying, leg.contract.expiry_time()}].push_back(&leg);
  Money total;
  for (const auto& [key, group] : groups) {
    auto requirement = verticals(group, OptionType::Put) + verticals(group, OptionType::Call);
    if (const auto loss = worst_loss(group)) requirement = std::min(requirement, *loss);
    total = total + requirement;
  }
  return total;
}
}  // namespace openport::trading

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
/// Shorts paired with longs of the same type that expire with them or later.
/// The most exposed shorts (puts from the highest strike down, calls from the
/// lowest up) each take the most protective eligible long left, the earliest
/// expiring on a tie. A pair costs its width, never more than naked; unpaired
/// shorts are naked.
Money verticals(const std::vector<const MarginLeg*>& legs, OptionType type) {
  struct Side { std::int64_t strike; Timestamp expiry; Quantity left; const MarginLeg* leg; };
  std::vector<Side> shorts, longs;
  for (const auto* leg : legs) {
    if (leg->contract.type != type) continue;
    auto& list = leg->quantity < 0 ? shorts : longs;
    list.push_back({milli(leg->contract.strike), leg->contract.expiry_time(), leg->quantity < 0 ? -leg->quantity : leg->quantity, leg});
  }
  const bool puts = type == OptionType::Put;
  const auto protects = [puts](std::int64_t a, std::int64_t b) { return puts ? a > b : a < b; };
  std::stable_sort(shorts.begin(), shorts.end(), [&](const Side& a, const Side& b) { return protects(a.strike, b.strike); });
  Money cost;
  for (auto& s : shorts) {
    while (s.left > 0) {
      Side* best = nullptr;
      for (auto& l : longs)
        if (l.left > 0 && l.expiry >= s.expiry &&
            (!best || protects(l.strike, best->strike) || (l.strike == best->strike && l.expiry < best->expiry)))
          best = &l;
      if (!best) break;
      const auto n = std::min(s.left, best->left);
      const auto width = std::max<std::int64_t>(0, puts ? s.strike - best->strike : best->strike - s.strike);
      cost = cost + std::min(Money::from_micros(width * 1000) * 100 * n, naked(*s.leg, n));
      s.left -= n;
      best->left -= n;
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

Quantity naked_shorts(const std::vector<MarginLeg>& legs) {
  // Per underlying and type, the latest-expiring shorts take the earliest long
  // that still covers them, which covers the most shorts overall.
  std::map<std::pair<std::string, OptionType>, std::pair<std::map<Timestamp, Quantity>, std::map<Timestamp, Quantity>>> books;
  for (const auto& leg : legs) {
    if (leg.quantity == 0) continue;
    auto& [shorts, longs] = books[{leg.contract.underlying, leg.contract.type}];
    (leg.quantity < 0 ? shorts : longs)[leg.contract.expiry_time()] += leg.quantity < 0 ? -leg.quantity : leg.quantity;
  }
  Quantity naked = 0;
  for (auto& [key, book] : books) {
    auto& [shorts, longs] = book;
    for (auto s = shorts.rbegin(); s != shorts.rend(); ++s) {
      auto left = s->second;
      for (auto l = longs.lower_bound(s->first); l != longs.end() && left > 0; ++l) {
        const auto n = std::min(left, l->second);
        left -= n;
        l->second -= n;
      }
      naked += left;
    }
  }
  return naked;
}

Money margin_requirement(const std::vector<MarginLeg>& legs) {
  std::map<std::string, std::vector<const MarginLeg*>> underlyings;
  for (const auto& leg : legs)
    if (leg.quantity != 0) underlyings[leg.contract.underlying].push_back(&leg);
  Money total;
  for (const auto& [underlying, all] : underlyings) {
    // Each expiry on its own: verticals, or the bounded worst loss at that expiry.
    std::map<Timestamp, std::vector<const MarginLeg*>> expiries;
    for (const auto* leg : all) expiries[leg->contract.expiry_time()].push_back(leg);
    Money separate;
    for (const auto& [expiry, group] : expiries) {
      auto requirement = verticals(group, OptionType::Put) + verticals(group, OptionType::Call);
      if (const auto loss = worst_loss(group)) requirement = std::min(requirement, *loss);
      separate = separate + requirement;
    }
    // Across expiries: a later long also covers an earlier short (calendars, diagonals).
    total = total + std::min(separate, verticals(all, OptionType::Put) + verticals(all, OptionType::Call));
  }
  return total;
}
}  // namespace openport::trading

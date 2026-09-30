#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

#include "openport/trading/evaluation.hpp"
#include "openport/trading/risk.hpp"

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
Quantity size(const MarginLeg& leg) { return leg.quantity < 0 ? -leg.quantity : leg.quantity; }
/// `n` contracts of a short paired with a long of its type that expires with it
/// or later: a put long below or a call long above its short costs the width.
struct Pair {
  const MarginLeg* short_leg;
  const MarginLeg* long_leg;
  Quantity n;
};
Money width(const Pair& p) {
  const auto a = milli(p.short_leg->contract.strike), b = milli(p.long_leg->contract.strike);
  const auto points = std::max<std::int64_t>(0, p.short_leg->contract.type == OptionType::Put ? a - b : b - a);
  return Money::from_micros(points * 1000) * 100 * p.n;
}
/// A pair never holds more than its short would naked.
Money cost(const Pair& p) { return std::min(width(p), naked(*p.short_leg, p.n)); }
struct Pairing {
  std::vector<Pair> pairs;
  std::vector<std::pair<const MarginLeg*, Quantity>> unpaired_shorts, unpaired_longs;
  /// The pairs' cost plus the unpaired shorts, naked.
  Money cost;
};
/// Pairs one type's shorts with its longs to hold the least in total: a
/// min-cost flow (successive shortest paths) where each contract a pair covers
/// saves its short's naked cost less the pair's width. Any short may take any
/// long that expires with it or later, so a greedy pass that lets one short take
/// the long another needed (across expiries, or on equal strikes) cannot happen.
Pairing verticals(const std::vector<const MarginLeg*>& legs, OptionType type) {
  std::vector<const MarginLeg*> shorts, longs;
  for (const auto* leg : legs)
    if (leg->contract.type == type && leg->quantity != 0) (leg->quantity < 0 ? shorts : longs).push_back(leg);
  // Nodes: source, shorts, longs, sink. Edge i's reverse is i ^ 1.
  struct Edge { std::size_t to; Quantity capacity; std::int64_t cost; };
  std::vector<Edge> edges;
  const std::size_t source = 0, sink = shorts.size() + longs.size() + 1;
  std::vector<std::vector<std::size_t>> out(sink + 1);
  const auto link = [&](std::size_t from, std::size_t to, Quantity capacity, std::int64_t cost) {
    out[from].push_back(edges.size());
    edges.push_back({to, capacity, cost});
    out[to].push_back(edges.size());
    edges.push_back({from, 0, -cost});
  };
  std::vector<std::size_t> matches;  // Short-to-long edges, in (short, long) order.
  for (std::size_t i = 0; i < shorts.size(); ++i) {
    link(source, 1 + i, size(*shorts[i]), 0);
    const auto naked_one = naked(*shorts[i], 1).micros();
    for (std::size_t j = 0; j < longs.size(); ++j) {
      if (longs[j]->contract.expiry_time() < shorts[i]->contract.expiry_time()) continue;
      const auto saving = naked_one - std::min(naked_one, width({shorts[i], longs[j], 1}).micros());
      if (saving <= 0) continue;
      matches.push_back(edges.size());
      link(1 + i, 1 + shorts.size() + j, std::min(size(*shorts[i]), size(*longs[j])), -saving);
    }
  }
  for (std::size_t j = 0; j < longs.size(); ++j) link(1 + shorts.size() + j, sink, size(*longs[j]), 0);
  // Augment along the cheapest path while it still saves something. The residual
  // graph never has a negative cycle, so Bellman-Ford finds it.
  constexpr auto unreached = std::numeric_limits<std::int64_t>::max();
  for (;;) {
    std::vector<std::int64_t> distance(sink + 1, unreached);
    std::vector<std::size_t> via(sink + 1, edges.size());
    distance[source] = 0;
    for (std::size_t round = 0; round <= sink; ++round) {
      bool changed = false;
      for (std::size_t node = 0; node <= sink; ++node) {
        if (distance[node] == unreached) continue;
        for (const auto e : out[node]) {
          const auto& edge = edges[e];
          if (edge.capacity > 0 && distance[node] + edge.cost < distance[edge.to]) {
            distance[edge.to] = distance[node] + edge.cost;
            via[edge.to] = e;
            changed = true;
          }
        }
      }
      if (!changed) break;
    }
    if (distance[sink] == unreached || distance[sink] >= 0) break;
    auto push = std::numeric_limits<Quantity>::max();
    for (auto node = sink; node != source; node = edges[via[node] ^ 1].to) push = std::min(push, edges[via[node]].capacity);
    for (auto node = sink; node != source; node = edges[via[node] ^ 1].to) {
      edges[via[node]].capacity -= push;
      edges[via[node] ^ 1].capacity += push;
    }
  }
  Pairing result;
  std::vector<Quantity> short_left(shorts.size()), long_left(longs.size());
  for (std::size_t i = 0; i < shorts.size(); ++i) short_left[i] = size(*shorts[i]);
  for (std::size_t j = 0; j < longs.size(); ++j) long_left[j] = size(*longs[j]);
  for (const auto e : matches) {
    const auto n = edges[e ^ 1].capacity;  // The flow carried.
    if (n == 0) continue;
    const auto i = edges[e ^ 1].to - 1, j = edges[e].to - 1 - shorts.size();
    result.pairs.push_back({shorts[i], longs[j], n});
    result.cost = result.cost + cost(result.pairs.back());
    short_left[i] -= n;
    long_left[j] -= n;
  }
  for (std::size_t i = 0; i < shorts.size(); ++i)
    if (short_left[i] > 0) {
      result.unpaired_shorts.emplace_back(shorts[i], short_left[i]);
      result.cost = result.cost + naked(*shorts[i], short_left[i]);
    }
  for (std::size_t j = 0; j < longs.size(); ++j)
    if (long_left[j] > 0) result.unpaired_longs.emplace_back(longs[j], long_left[j]);
  return result;
}
/// The worst loss at `expiry` of pairs whose shorts all expire then, with that
/// expiry's unpaired longs: a later long counts at its intrinsic value there.
/// Pairs cannot be net short calls, so the minimum is at zero or a strike.
Money paired_worst_loss(const std::vector<Pair>& pairs, const std::vector<std::pair<const MarginLeg*, Quantity>>& longs) {
  std::vector<std::int64_t> spots{0};
  for (const auto& p : pairs) {
    spots.push_back(milli(p.short_leg->contract.strike));
    spots.push_back(milli(p.long_leg->contract.strike));
  }
  for (const auto& [leg, n] : longs) spots.push_back(milli(leg->contract.strike));
  Money worst;
  for (const auto spot : spots) {
    Money payoff;
    for (const auto& p : pairs)
      payoff = payoff + (intrinsic(p.long_leg->contract, spot) - intrinsic(p.short_leg->contract, spot)) * p.n;
    for (const auto& [leg, n] : longs) payoff = payoff + intrinsic(leg->contract, spot) * n;
    worst = std::min(worst, payoff);
  }
  return -worst;
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
      auto requirement = verticals(group, OptionType::Put).cost + verticals(group, OptionType::Call).cost;
      if (const auto loss = worst_loss(group)) requirement = std::min(requirement, *loss);
      separate = separate + requirement;
    }
    // Across expiries: a later long also covers an earlier short (calendars,
    // diagonals). The pairs whose shorts expire together lose at that expiry
    // at most their combined worst loss, so an iron condor beside a calendar
    // still holds only its wider wing.
    Money across;
    std::map<Timestamp, std::pair<std::vector<Pair>, std::vector<std::pair<const MarginLeg*, Quantity>>>> by_expiry;
    for (const auto type : {OptionType::Put, OptionType::Call}) {
      auto pairing = verticals(all, type);
      for (const auto& [leg, n] : pairing.unpaired_shorts) across = across + naked(*leg, n);
      for (const auto& p : pairing.pairs) by_expiry[p.short_leg->contract.expiry_time()].first.push_back(p);
      for (const auto& [leg, n] : pairing.unpaired_longs) by_expiry[leg->contract.expiry_time()].second.emplace_back(leg, n);
    }
    for (const auto& [expiry, group] : by_expiry) {
      const auto& [pairs, longs] = group;
      if (pairs.empty()) continue;
      Money paired;
      for (const auto& p : pairs) paired = paired + cost(p);
      across = across + std::min(paired, paired_worst_loss(pairs, longs));
    }
    total = total + std::min(separate, across);
  }
  return total;
}

std::optional<Money> portfolio_margin_requirement(const std::vector<MarginLeg>& legs,
    const Valuations& valuations, Timestamp now, Timestamp max_age,
    const std::map<std::string, StockPosition>& stocks, const std::map<std::string, double>& stock_prices) {
  struct Group {
    std::map<std::string, Position> options;
    std::map<std::string, StockPosition> stocks;
    Money minimum;
  };
  std::map<std::string, Group> groups;
  for (const auto& leg : legs) {
    if (leg.quantity == 0) continue;
    auto& group = groups[leg.contract.underlying];
    group.options.emplace(leg.contract.osi_symbol(), Position{leg.contract, leg.quantity, {}, {}, {}});
    group.minimum = group.minimum + Money::from_double(0.375 * leg.contract.multiplier) *
        (leg.quantity < 0 ? -leg.quantity : leg.quantity);
  }
  for (const auto& [symbol, stock] : stocks) groups[symbol].stocks.emplace(symbol, stock);
  Money total;
  for (const auto& [underlying, group] : groups) {
    ScenarioConfig scan;
    scan.spot_percent.clear();
    scan.vol_points = {0};
    // The account's display grid cannot narrow the margin scan.
    const bool index = md::is_index_underlying(underlying);
    const double low = index ? -8.0 : -15.0;
    const double high = index ? 6.0 : 15.0;
    for (int i = 0; i <= 10; ++i) scan.spot_percent.push_back(low + (high - low) * i / 10);
    // Keep the valuation's IV, even when it is below the display grid's floor.
    scan.vol_floor = std::numeric_limits<double>::min();
    const auto ledger = Ledger::restore({}, group.options, group.stocks);
    const auto grid = scenario_grid(ledger, valuations, scan, now, max_age, stock_prices);
    if (!grid.complete) return std::nullopt;
    double loss = 0;
    for (const auto& cell : grid.cells) loss = std::max(loss, -cell.pnl);
    // The minimum is a floor under the scanned loss, not an addition to it.
    total = total + std::max(Money::from_double(loss), group.minimum);
  }
  return total;
}
}  // namespace openport::trading

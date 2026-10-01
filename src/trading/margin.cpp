#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <functional>
#include <map>
#include <span>
#include <vector>

#include "openport/trading/evaluation.hpp"
#include "openport/trading/risk.hpp"

namespace openport::trading {
namespace {
using pricing::OptionType;
constexpr Quantity kLot = 100;  ///< Shares one contract delivers, and so covers.
/// Strikes are validated to whole thousandths of a dollar.
std::int64_t milli(double strike) { return std::llround(strike * 1000); }
Quantity magnitude(Quantity q) { return q < 0 ? -q : q; }
/// One contract's intrinsic value at expiry, times the multiplier, at a spot in thousandths.
Money intrinsic(const md::OptionContract& c, std::int64_t spot) {
  const auto strike = milli(c.strike);
  const auto amount = std::max<std::int64_t>(0, c.type == OptionType::Call ? spot - strike : strike - spot);
  return Money::from_micros(amount * 1000) * 100;
}
/// `shares` of a short stock position hold their value and half again, as a short sale does.
Money short_shares(const MarginStock& stock, Quantity shares) {
  return stock.value.prorate(shares, magnitude(stock.shares)).prorate(3, 2);
}
/// A position as strategy margin pairs it: an option leg, or whole lots of 100
/// shares. Long shares cover a short call and short shares a short put, whatever
/// either expires; short shares are also a short of their own that a long call
/// can protect.
struct Unit {
  const MarginLeg* leg = nullptr;      ///< Null for shares.
  const MarginStock* stock = nullptr;  ///< The shares, for lots.
  Quantity size = 0;                   ///< Contracts, or lots.
  Money requirement_one;               ///< An option short's naked requirement for one contract, without its value.
  Money naked_one;                     ///< What one contract or lot of a short holds unpaired.
};
/// The buy-back value of `n` of a short option's contracts.
Money value(const Unit& u, Quantity n) { return u.leg->value.prorate(n, -u.leg->quantity); }
/// What `n` contracts or lots of a short hold unpaired: an option its buy-back
/// value plus its naked requirement, shares their value and half again.
Money naked(const Unit& u, Quantity n) {
  if (u.stock) return short_shares(*u.stock, n * kLot);
  return value(u, n) + u.requirement_one * n;
}
/// What `n` of short `s` hold with the long or shares that cover them: a put
/// long below or a call long above its short the strikes' distance (nothing when
/// the long is at or beyond the short's strike), a long call protecting short
/// shares its strike, long shares written against a call nothing, and short
/// shares written against a put the put's buy-back value, so its credit stays.
Money covered(const Unit& s, const Unit& cover, Quantity n) {
  if (cover.stock) return cover.stock->shares > 0 ? Money{} : value(s, n);
  const auto b = milli(cover.leg->contract.strike);
  const auto a = s.stock ? 0 : milli(s.leg->contract.strike);
  const bool put = s.leg && s.leg->contract.type == OptionType::Put;
  const auto points = std::max<std::int64_t>(0, put ? a - b : b - a);
  return Money::from_micros(points * 1000) * 100 * n;
}
/// Whether `cover` can pair with short `s`: shares cover an option whatever it
/// expires, a long call protects short shares while it lasts, and a long option
/// covers a short of its type that expires with it or earlier.
bool covers(const Unit& cover, const Unit& s) {
  if (cover.stock || s.stock) return true;
  return cover.leg->contract.expiry_time() >= s.leg->contract.expiry_time();
}
/// Reg T's short straddle or combination: the greater naked requirement, plus
/// the other side's buy-back value (the larger one when both need the same).
Money straddle(const Unit& put, const Unit& call, Quantity n) {
  const auto p = naked(put, n), c = naked(call, n);
  if (p != c) return p > c ? p + value(call, n) : c + value(put, n);
  return p + std::max(value(put, n), value(call, n));
}
/// What one contract of a short put and a short call save as a straddle.
std::int64_t straddle_saving(const Unit& put, const Unit& call) {
  return (put.naked_one + call.naked_one - straddle(put, call, 1)).micros();
}
/// `n` of a short with the long or shares that cover it, or a short put with
/// the short call of its straddle.
struct Pair {
  const Unit* short_unit;
  const Unit* cover;  ///< The short call, for a straddle.
  Quantity n;
  bool straddle = false;
};
/// Two options, not shares: pairs whose shorts expire together share a worst loss.
bool vertical(const Pair& p) { return !p.straddle && p.short_unit->leg && p.cover->leg; }
/// A pair never holds more than its short would naked.
Money cost(const Pair& p) {
  if (p.straddle) return straddle(*p.short_unit, *p.cover, p.n);
  return std::min(covered(*p.short_unit, *p.cover, p.n), naked(*p.short_unit, p.n));
}
/// One underlying's positions, sorted for pairing. Short shares sit both among
/// the short calls (a short a long call protects) and the long puts (a cover).
struct Book {
  std::vector<Unit> short_puts, long_calls, short_calls, long_puts;
};
Book book_of(const std::vector<const MarginLeg*>& legs, const MarginStock* stock) {
  Book book;
  for (const auto* leg : legs) {
    Unit u{leg, nullptr, magnitude(leg->quantity), {}, {}};
    const bool put = leg->contract.type == OptionType::Put;
    if (leg->quantity < 0) {
      u.requirement_one = naked_requirement(leg->contract, leg->spot);
      u.naked_one = naked(u, 1);
      (put ? book.short_puts : book.short_calls).push_back(u);
    } else {
      (put ? book.long_puts : book.long_calls).push_back(u);
    }
  }
  if (stock && magnitude(stock->shares) >= kLot) {
    Unit lots{nullptr, stock, magnitude(stock->shares) / kLot, {}, {}};
    if (stock->shares > 0) {
      book.long_calls.push_back(lots);
    } else {
      lots.naked_one = naked(lots, 1);
      book.short_calls.push_back(lots);
      book.long_puts.push_back(lots);
    }
  }
  return book;
}
struct Pairing {
  std::vector<Pair> pairs;
  std::vector<std::pair<const Unit*, Quantity>> unpaired_shorts, unpaired_longs;
};
/// Pairs one type's shorts with the longs (or shares) that cover them to hold
/// the least in total: a min-cost flow (successive shortest paths) where each
/// contract a pair covers saves its short's naked cost less the pair's cost.
/// Any short may take any cover that lasts as long, so a greedy pass that lets
/// one short take the cover another needed (across expiries, or on equal
/// strikes) cannot happen.
Pairing verticals(std::span<const Unit> shorts, std::span<const Unit> longs) {
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
    link(source, 1 + i, shorts[i].size, 0);
    const auto naked_one = shorts[i].naked_one.micros();
    for (std::size_t j = 0; j < longs.size(); ++j) {
      if (!covers(longs[j], shorts[i])) continue;
      const auto saving = naked_one - std::min(naked_one, covered(shorts[i], longs[j], 1).micros());
      if (saving <= 0) continue;
      matches.push_back(edges.size());
      link(1 + i, 1 + shorts.size() + j, std::min(shorts[i].size, longs[j].size), -saving);
    }
  }
  for (std::size_t j = 0; j < longs.size(); ++j) link(1 + shorts.size() + j, sink, longs[j].size, 0);
  // Augment along the cheapest path while it still saves something. The residual
  // graph never has a negative cycle, so a queue-based Bellman-Ford finds it.
  constexpr auto unreached = std::numeric_limits<std::int64_t>::max();
  std::vector<std::int64_t> distance(sink + 1);
  std::vector<std::size_t> via(sink + 1);
  std::vector<char> queued(sink + 1);
  std::deque<std::size_t> queue;
  while (!matches.empty()) {
    std::fill(distance.begin(), distance.end(), unreached);
    distance[source] = 0;
    queue.assign(1, source);
    queued[source] = 1;
    while (!queue.empty()) {
      const auto node = queue.front();
      queue.pop_front();
      queued[node] = 0;
      for (const auto e : out[node]) {
        const auto& edge = edges[e];
        if (edge.capacity <= 0 || distance[node] + edge.cost >= distance[edge.to]) continue;
        distance[edge.to] = distance[node] + edge.cost;
        via[edge.to] = e;
        if (!queued[edge.to]) { queue.push_back(edge.to); queued[edge.to] = 1; }
      }
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
  for (std::size_t i = 0; i < shorts.size(); ++i) short_left[i] = shorts[i].size;
  for (std::size_t j = 0; j < longs.size(); ++j) long_left[j] = longs[j].size;
  for (const auto e : matches) {
    const auto n = edges[e ^ 1].capacity;  // The flow carried.
    if (n == 0) continue;
    const auto i = edges[e ^ 1].to - 1, j = edges[e].to - 1 - shorts.size();
    result.pairs.push_back({&shorts[i], &longs[j], n});
    short_left[i] -= n;
    long_left[j] -= n;
  }
  for (std::size_t i = 0; i < shorts.size(); ++i)
    if (short_left[i] > 0) result.unpaired_shorts.emplace_back(&shorts[i], short_left[i]);
  for (std::size_t j = 0; j < longs.size(); ++j)
    if (long_left[j] > 0) result.unpaired_longs.emplace_back(&longs[j], long_left[j]);
  return result;
}
/// What one contract of a vertical saves against its short naked.
std::int64_t saving_one(const Pair& p) {
  const auto naked_one = p.short_unit->naked_one.micros();
  return naked_one - std::min(naked_one, covered(*p.short_unit, *p.cover, 1).micros());
}
/// Short puts and short calls that the verticals left naked pair as straddles,
/// the greatest naked requirement of each type together, which saves the most
/// when the larger requirement comes with the larger value, as it usually does.
/// The shorts of one type still naked then take, greatest first, the short of
/// a vertical of the other type where a straddle saves more than that vertical
/// did; its long goes unpaired.
void add_straddles(Pairing& pairing) {
  std::vector<std::pair<const Unit*, Quantity>> puts, calls, kept;
  for (const auto& entry : pairing.unpaired_shorts) {
    if (!entry.first->leg) kept.push_back(entry);
    else (entry.first->leg->contract.type == OptionType::Put ? puts : calls).push_back(entry);
  }
  if (puts.empty() && calls.empty()) return;
  const auto greater = [](const auto& a, const auto& b) { return a.first->naked_one > b.first->naked_one; };
  std::stable_sort(puts.begin(), puts.end(), greater);
  std::stable_sort(calls.begin(), calls.end(), greater);
  std::size_t a = 0, b = 0;
  while (a < puts.size() && b < calls.size()) {
    const auto n = std::min(puts[a].second, calls[b].second);
    pairing.pairs.push_back({puts[a].first, calls[b].first, n, true});
    if ((puts[a].second -= n) == 0) ++a;
    if ((calls[b].second -= n) == 0) ++b;
  }
  auto& still_naked = a < puts.size() ? puts : calls;
  for (auto i = a < puts.size() ? a : b; i < still_naked.size(); ++i) {
    auto& [unit, left] = still_naked[i];
    const bool put = unit->leg->contract.type == OptionType::Put;
    while (left > 0) {
      Pair* best = nullptr;
      std::int64_t gain = 0;
      for (auto& p : pairing.pairs) {
        if (p.straddle || p.n == 0 || !p.short_unit->leg || (p.short_unit->leg->contract.type == OptionType::Put) == put) continue;
        const auto saving = put ? straddle_saving(*unit, *p.short_unit) : straddle_saving(*p.short_unit, *unit);
        if (const auto value = saving - saving_one(p); value > gain) { gain = value; best = &p; }
      }
      if (!best) break;
      const auto n = std::min(left, best->n);
      best->n -= n;
      pairing.unpaired_longs.emplace_back(best->cover, n);
      pairing.pairs.push_back({put ? unit : best->short_unit, put ? best->short_unit : unit, n, true});
      left -= n;
    }
  }
  std::erase_if(pairing.pairs, [](const Pair& p) { return p.n == 0; });
  pairing.unpaired_shorts = std::move(kept);
  for (const auto* side : {&puts, &calls})
    for (const auto& entry : *side)
      if (entry.second > 0) pairing.unpaired_shorts.push_back(entry);
}
/// Pairs each type's shorts as verticals.
Pairing pair_units(const Book& book) {
  auto result = verticals(book.short_puts, book.long_puts);
  auto calls = verticals(book.short_calls, book.long_calls);
  result.pairs.insert(result.pairs.end(), calls.pairs.begin(), calls.pairs.end());
  result.unpaired_shorts.insert(result.unpaired_shorts.end(), calls.unpaired_shorts.begin(), calls.unpaired_shorts.end());
  result.unpaired_longs.insert(result.unpaired_longs.end(), calls.unpaired_longs.begin(), calls.unpaired_longs.end());
  return result;
}
/// The worst loss at `expiry` of pairs whose shorts all expire then, with that
/// expiry's unpaired longs: a later long counts at its intrinsic value there.
/// Pairs cannot be net short calls, so the minimum is at zero or a strike.
Money paired_worst_loss(const std::vector<const Pair*>& pairs, const std::vector<std::pair<const Unit*, Quantity>>& longs) {
  std::vector<std::int64_t> spots{0};
  for (const auto* p : pairs) {
    spots.push_back(milli(p->short_unit->leg->contract.strike));
    spots.push_back(milli(p->cover->leg->contract.strike));
  }
  for (const auto& [unit, n] : longs) spots.push_back(milli(unit->leg->contract.strike));
  Money worst;
  for (const auto spot : spots) {
    Money payoff;
    for (const auto* p : pairs)
      payoff = payoff + (intrinsic(p->cover->leg->contract, spot) - intrinsic(p->short_unit->leg->contract, spot)) * p->n;
    for (const auto& [unit, n] : longs) payoff = payoff + intrinsic(unit->leg->contract, spot) * n;
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
using Parts = std::vector<MarginPart>;
std::pair<std::string, Quantity> shares_leg(const MarginStock& stock, Quantity shares) {
  return {stock.underlying, stock.shares < 0 ? -shares : shares};
}
std::pair<std::string, Quantity> option_leg(const Unit& u, Quantity n) {
  return {u.leg->contract.osi_symbol(), u.leg->quantity < 0 ? -n : n};
}
/// One leg of a part: an option's contracts, or a lot's shares.
std::pair<std::string, Quantity> leg_of(const Unit& u, Quantity n) {
  return u.stock ? shares_leg(*u.stock, n * kLot) : option_leg(u, n);
}
void add(Parts* parts, MarginPartKind kind, std::vector<std::pair<std::string, Quantity>> legs, Money requirement) {
  if (parts) parts->push_back({kind, std::move(legs), requirement});
}
/// Longs no part holds: their premium is paid in full, as long shares are.
void add_longs(Parts* parts, const std::vector<std::pair<const Unit*, Quantity>>& longs) {
  for (const auto& [unit, n] : longs)
    if (unit->leg) add(parts, MarginPartKind::Long, {leg_of(*unit, n)}, {});
}
/// Shares that no pair takes: short ones hold their value and half again, long ones are paid for.
Money other_shares(const MarginStock* stock, Quantity lots_taken, Parts* parts) {
  if (!stock) return {};
  const auto shares = magnitude(stock->shares) - lots_taken * kLot;
  if (shares <= 0) return {};
  if (stock->shares > 0) {
    add(parts, MarginPartKind::Long, {shares_leg(*stock, shares)}, {});
    return {};
  }
  const auto requirement = short_shares(*stock, shares);
  add(parts, MarginPartKind::ShortShares, {shares_leg(*stock, shares)}, requirement);
  return requirement;
}
/// What the book holds when its shorts pair across expiries (`pairing`): each
/// pair its cost, the verticals whose shorts expire together at most their
/// combined worst loss then, and unpaired shorts naked.
Money across(const Pairing& pairing, const MarginStock* stock, Parts* parts) {
  Money total;
  Quantity lots_taken = 0;  // Short shares a long call protects, or long shares a call is written against.
  std::map<Timestamp, std::pair<std::vector<const Pair*>, std::vector<std::pair<const Unit*, Quantity>>>> by_expiry;
  for (const auto& p : pairing.pairs) {
    if (vertical(p)) {
      by_expiry[p.short_unit->leg->contract.expiry_time()].first.push_back(&p);
      continue;
    }
    const auto requirement = cost(p);
    total = total + requirement;
    if (p.short_unit->stock || (p.cover->stock && p.cover->stock->shares > 0)) lots_taken += p.n;
    add(parts, p.straddle ? MarginPartKind::Straddle : p.short_unit->stock ? MarginPartKind::ProtectedShares : MarginPartKind::Covered,
        {leg_of(*p.short_unit, p.n), leg_of(*p.cover, p.n)}, requirement);
  }
  for (const auto& [unit, n] : pairing.unpaired_longs)
    if (unit->leg) by_expiry[unit->leg->contract.expiry_time()].second.emplace_back(unit, n);
  for (const auto& [expiry, group] : by_expiry) {
    const auto& [pairs, longs] = group;
    if (pairs.empty()) {
      add_longs(parts, longs);
      continue;
    }
    Money paired;
    for (const auto* p : pairs) paired = paired + cost(*p);
    const auto worst = paired_worst_loss(pairs, longs);
    total = total + std::min(paired, worst);
    if (!parts) continue;
    if (worst < paired) {
      std::vector<std::pair<std::string, Quantity>> legs;
      for (const auto* p : pairs) {
        legs.push_back(leg_of(*p->short_unit, p->n));
        legs.push_back(leg_of(*p->cover, p->n));
      }
      for (const auto& [unit, n] : longs) legs.push_back(leg_of(*unit, n));
      add(parts, MarginPartKind::WorstLoss, std::move(legs), worst);
    } else {
      for (const auto* p : pairs) add(parts, MarginPartKind::Vertical, {leg_of(*p->short_unit, p->n), leg_of(*p->cover, p->n)}, cost(*p));
      add_longs(parts, longs);
    }
  }
  for (const auto& [unit, n] : pairing.unpaired_shorts) {
    if (unit->stock) continue;  // Short shares hold below.
    const auto requirement = naked(*unit, n);
    total = total + requirement;
    add(parts, MarginPartKind::Naked, {leg_of(*unit, n)}, requirement);
  }
  return total + other_shares(stock, lots_taken, parts);
}
/// What the book holds when each expiry stands on its own: its verticals and
/// naked shorts, or its worst loss at expiry when that is bounded and less.
/// Shares expire with none of them, so here they cover nothing.
Money separate(const std::vector<const MarginLeg*>& all, const MarginStock* stock, Parts* parts) {
  std::map<Timestamp, std::vector<const MarginLeg*>> expiries;
  for (const auto* leg : all) expiries[leg->contract.expiry_time()].push_back(leg);
  Money total;
  for (const auto& [expiry, group] : expiries) {
    const auto book = book_of(group, nullptr);
    Parts own;
    Parts* explain = parts ? &own : nullptr;
    const auto pairing = pair_units(book);
    Money requirement;
    for (const auto& p : pairing.pairs) {
      requirement = requirement + cost(p);
      add(explain, MarginPartKind::Vertical, {leg_of(*p.short_unit, p.n), leg_of(*p.cover, p.n)}, cost(p));
    }
    for (const auto& [unit, n] : pairing.unpaired_shorts) {
      requirement = requirement + naked(*unit, n);
      add(explain, MarginPartKind::Naked, {leg_of(*unit, n)}, naked(*unit, n));
    }
    add_longs(explain, pairing.unpaired_longs);
    if (const auto loss = worst_loss(group); loss && *loss < requirement) {
      requirement = *loss;
      own.clear();
      std::vector<std::pair<std::string, Quantity>> legs;
      for (const auto* leg : group) legs.emplace_back(leg->contract.osi_symbol(), leg->quantity);
      add(explain, MarginPartKind::WorstLoss, std::move(legs), *loss);
    }
    total = total + requirement;
    if (parts) parts->insert(parts->end(), own.begin(), own.end());
  }
  return total + other_shares(stock, 0, parts);
}
/// One underlying's requirement: the least of pairing across expiries, as
/// verticals and, when that leaves a short option naked, with straddles too (a
/// condor's shared worst loss may still hold less without them), and taking
/// each expiry on its own.
Money underlying_requirement(const std::vector<const MarginLeg*>& all, const MarginStock* stock, Parts* parts) {
  const auto book = book_of(all, stock);
  const auto pairing = pair_units(book);
  Parts best_parts, candidate;
  Parts* explain = parts ? &candidate : nullptr;
  auto best = across(pairing, stock, explain);
  best_parts.swap(candidate);
  const auto consider = [&](Money requirement) {
    if (requirement < best) { best = requirement; best_parts.swap(candidate); }
    candidate.clear();
  };
  const auto option = [](const auto& entry) { return entry.first->leg != nullptr; };
  if (!book.short_puts.empty() && std::any_of(book.short_calls.begin(), book.short_calls.end(), [](const Unit& u) { return u.leg != nullptr; }) &&
      std::any_of(pairing.unpaired_shorts.begin(), pairing.unpaired_shorts.end(), option)) {
    auto straddled = pairing;
    add_straddles(straddled);
    consider(across(straddled, stock, explain));
  }
  consider(separate(all, stock, explain));
  if (parts) *parts = std::move(best_parts);
  return best;
}
/// Strategy margin by underlying, shares joining their own.
std::vector<MarginUnderlying> strategy(const std::vector<MarginLeg>& legs, const std::vector<MarginStock>& stocks, bool explain) {
  std::map<std::string, std::pair<std::vector<const MarginLeg*>, const MarginStock*>> underlyings;
  for (const auto& leg : legs)
    if (leg.quantity != 0) underlyings[leg.contract.underlying].first.push_back(&leg);
  for (const auto& stock : stocks)
    if (stock.shares != 0) underlyings[stock.underlying].second = &stock;
  std::vector<MarginUnderlying> result;
  for (const auto& [underlying, group] : underlyings) {
    MarginUnderlying item;
    item.underlying = underlying;
    item.requirement = underlying_requirement(group.first, group.second, explain ? &item.parts : nullptr);
    result.push_back(std::move(item));
  }
  return result;
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

Money margin_requirement(const std::vector<MarginLeg>& legs, const std::vector<MarginStock>& stocks) {
  Money total;
  for (const auto& item : strategy(legs, stocks, false)) total = total + item.requirement;
  return total;
}

std::vector<MarginUnderlying> margin_breakdown(const std::vector<MarginLeg>& legs, const std::vector<MarginStock>& stocks) {
  return strategy(legs, stocks, true);
}

std::optional<std::vector<MarginUnderlying>> portfolio_margin_breakdown(const std::vector<MarginLeg>& legs,
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
  std::vector<MarginUnderlying> result;
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
    PortfolioScan worst;
    for (const auto& cell : grid.cells)
      if (-cell.pnl > loss) {
        loss = -cell.pnl;
        worst.spot_percent = cell.spot_percent;
        worst.vol_points = cell.vol_points;
      }
    worst.loss = Money::from_double(loss);
    worst.minimum = group.minimum;
    MarginUnderlying item;
    item.underlying = underlying;
    // The minimum is a floor under the scanned loss, not an addition to it.
    item.requirement = std::max(worst.loss, group.minimum);
    item.scan = worst;
    result.push_back(std::move(item));
  }
  return result;
}

std::optional<Money> portfolio_margin_requirement(const std::vector<MarginLeg>& legs,
    const Valuations& valuations, Timestamp now, Timestamp max_age,
    const std::map<std::string, StockPosition>& stocks, const std::map<std::string, double>& stock_prices) {
  const auto groups = portfolio_margin_breakdown(legs, valuations, now, max_age, stocks, stock_prices);
  if (!groups) return std::nullopt;
  Money total;
  for (const auto& item : *groups) total = total + item.requirement;
  return total;
}
}  // namespace openport::trading

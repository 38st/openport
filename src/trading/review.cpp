#include "review.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

#include "openport/trading/history.hpp"

namespace openport::trading::detail {
namespace {
/// The settlement reference of a round trip settled at this instant: its
/// underlying's price for the last sample, which the contract's last valuation,
/// from before its expiry, does not give.
std::optional<double> settled_spot(const State& s, const Lifecycle& life) {
  if (life.closure != ClosureKind::Settlement || life.closed != s.time) return {};
  const auto it = s.settling.find(life.symbol);
  return it == s.settling.end() ? std::nullopt : std::optional(it->second.dollars());
}
std::optional<double> spot(const State& s, const Lifecycle& life) {
  if (const auto settled = settled_spot(s, life)) return settled;
  const auto v = s.valuations.find(life.symbol);
  if (v != s.valuations.end() && valid_valuation(v->second) &&
      s.time - v->second.time <= s.config.limits.max_valuation_age) return v->second.spot;
  const auto stock = s.stock_marks.find(life.contract.underlying);
  if (stock != s.stock_marks.end() && s.time - stock->second.time <= s.config.limits.max_quote_age)
    return stock->second.price.dollars();
  return {};
}
std::optional<Money> pnl(const State& s, const Lifecycle& life) {
  Money result = life.gross - life.fees;
  if (life.quantity != 0) {
    const auto mark = s.marks.find(life.symbol);
    if (mark == s.marks.end()) return {};
    result = result + (mark->second.price * life.contract.multiplier) * life.quantity - life.basis;
  }
  return result;
}
void sample(TradeReview& review, Money value, Timestamp time, std::optional<double> underlying, bool finished) {
  if (!review.worst || value < review.worst->pnl) review.worst = Excursion{value, time, underlying};
  if (!review.best || value > review.best->pnl) review.best = Excursion{value, time, underlying};
  review.finished = finished;
}
// Only entries carrying the new rule add state; historical reviews keep their
// exact encoding. The peak and first trigger survive recovery with the review.
void sample_trailing(State& s, TradeReview& review, const Order& order, const std::vector<const Lifecycle*>& legs) {
  if (review.finished || order.request.note.find("\"trailing_stop\"") == std::string::npos) return;
  const auto evidence = Json::parse(order.request.note, nullptr, false);
  if (!evidence.is_object() || !evidence.contains("trailing_stop") || !evidence.at("trailing_stop").is_object()) return;
  const auto& rule = evidence.at("trailing_stop");
  if (!rule.contains("percent") || !rule.at("percent").is_number()) return;
  const double percent = rule.at("percent");
  if (!std::isfinite(percent) || percent <= 0 || percent > 100) return;
  Money total;
  for (const auto* life : legs) {
    total = total + life->gross;
    if (!life->quantity) continue;
    const auto book = s.books.find(life->symbol);
    if (book == s.books.end()) return;
    const auto& quote = book->second.quote;
    if (!valid_quote(quote) || quote.time > s.time || s.time - quote.time > s.config.limits.max_quote_age) return;
    const auto price = life->quantity > 0 ? quote.bid : quote.ask;
    if (!price || *price <= Money{}) return;
    total = total + (*price * life->contract.multiplier) * life->quantity - life->basis;
  }
  if (!review.trailing) review.trailing = TrailingReview{};
  auto& trail = *review.trailing;
  if (trail.triggered) return;
  trail.peak = std::max(trail.peak, total);
  const auto fraction = static_cast<std::int64_t>(std::llround(percent * 1000000));
  if (trail.peak > Money{} && total <= trail.peak.prorate(100000000 - fraction, 100000000)) trail.triggered = s.time;
}
bool unfinished_review(const SharedMap<std::string, TradeReview>& reviews, const std::string& key) {
  const auto found = reviews.find(key);
  return found == reviews.end() || !found->second.finished;
}
const Order* entry_order(const State& s, const Lifecycle& life) {
  if (life.first_fill == 0 || life.first_fill > s.fills.size()) return nullptr;
  const auto id = s.fills[life.first_fill - 1].order_id;
  return id > 0 && id <= s.orders.size() ? &s.orders[id - 1] : nullptr;
}
std::optional<Money> stop_risk(const Lifecycle& life, const Order& order) {
  if (!order.request.bracket || !order.request.bracket->stop_loss) return {};
  const auto& stop = order.request.bracket->stop_loss->trigger;
  // An underlying level does not specify the option's execution price.
  if (!stop || stop->source != TriggerSource::Option || life.entry_contracts <= 0) return {};
  // The entry's distance to its stop, per contract, for every contract opened:
  // an add, wherever it fills, keeps the plan's risk per contract.
  const Money entry = life.entry_notional.prorate(life.opened_contracts, life.entry_contracts);
  const Money difference = life.direction > 0
      ? entry - stop->level * life.opened_contracts
      : stop->level * life.opened_contracts - entry;
  const Money risk = difference * life.contract.multiplier;
  return risk > Money{} ? std::optional(risk) : std::nullopt;
}
/// Exact terminal payoff minimum for a structure with one settlement instant.
/// Across expiries there is no single terminal payoff; leave that risk unknown.
std::optional<Money> structure_risk(const std::vector<const Lifecycle*>& legs) {
  const auto& first = legs.front()->contract;
  std::vector<Money> strikes{Money{}};
  Money debit;
  Quantity call_slope = 0;
  for (const auto* leg : legs) {
    const auto& c = leg->contract;
    if (c.expiry_time() != first.expiry_time()) return {};
    strikes.push_back(Money::from_double(c.strike));
    debit = debit + (leg->open_notional * c.multiplier) * leg->direction;
    if (c.type == pricing::OptionType::Call) call_slope += leg->direction * leg->opened_contracts * c.multiplier;
  }
  if (call_slope < 0) return {};
  std::optional<Money> worst;
  for (const auto price : strikes) {
    Money payoff;
    for (const auto* leg : legs) {
      const auto strike = Money::from_double(leg->contract.strike);
      const auto intrinsic = std::max(Money{}, leg->contract.type == pricing::OptionType::Call ? price - strike : strike - price);
      payoff = payoff + (intrinsic * leg->contract.multiplier) * (leg->direction * leg->opened_contracts);
    }
    const auto value = payoff - debit;
    worst = worst ? std::min(*worst, value) : value;
  }
  return worst && *worst < Money{} ? std::optional(-*worst) : std::nullopt;
}
/// A strategy's entry bracket stop on its combo price: the entry net's distance to
/// the stop level, per unit, for every unit its entry order opened, times the
/// multiplier. Both nets are signed as order prices, a debit positive, so a unit
/// closed at the stop loses their sum.
std::optional<Money> combo_stop_risk(const std::vector<const Lifecycle*>& legs, const Order& order) {
  if (!order.request.bracket || !order.request.bracket->stop_loss) return {};
  const auto& stop = order.request.bracket->stop_loss->trigger;
  if (!stop || stop->source != TriggerSource::Combo) return {};
  Money paid;
  Quantity units = 0;
  for (const auto* leg : legs) {
    const auto spec = std::find_if(order.request.legs.begin(), order.request.legs.end(), [&](const Leg& l) { return l.symbol == leg->symbol; });
    if (spec == order.request.legs.end() || leg->entry_contracts <= 0 || leg->entry_contracts % spec->ratio != 0) return {};
    const auto leg_units = leg->entry_contracts / spec->ratio;
    if (units != 0 && leg_units != units) return {};
    units = leg_units;
    paid = paid + leg->entry_notional * leg->direction;
  }
  const Money risk = (paid + stop->level * units) * legs.front()->contract.multiplier;
  return risk > Money{} ? std::optional(risk) : std::nullopt;
}
/// A calendar's or diagonal's debit, when every short is covered: by longs of its
/// type that expire with it or later, at a strike at least as good (a call long at
/// or below its short's strike, a put long at or above), contract for contract.
/// The longs are then worth at least what the shorts owe when they expire, so the
/// debit is what the structure can lose, as traders plan a calendar's risk.
std::optional<Money> covered_debit(const std::vector<const Lifecycle*>& legs) {
  std::vector<std::pair<const Lifecycle*, Quantity>> longs;
  Money debit;
  for (const auto* leg : legs) {
    debit = debit + (leg->open_notional * leg->contract.multiplier) * leg->direction;
    if (leg->direction > 0) longs.emplace_back(leg, leg->opened_contracts);
  }
  for (const auto* leg : legs) {
    if (leg->direction > 0) continue;
    auto owed = leg->opened_contracts;
    for (auto& [cover, left] : longs) {
      const auto& c = cover->contract;
      const bool call = c.type == pricing::OptionType::Call;
      if (c.type != leg->contract.type || c.expiry_time() < leg->contract.expiry_time() ||
          (call ? c.strike > leg->contract.strike : c.strike < leg->contract.strike)) continue;
      const auto used = std::min(owed, left);
      owed -= used;
      left -= used;
    }
    if (owed > 0) return {};
  }
  return debit > Money{} ? std::optional(debit) : std::nullopt;
}
/// Samples the reviews of `lives`, given in the order they opened, of the
/// multi-leg entries among them and of the trades with a whole-trade review. A
/// finished review stays as it is; a missing trade or strategy review starts now
/// (a whole-trade review starts only when a trade takes another entry).
/// `unfinished` changes by the change in unfinished reviews. `shared` are the
/// orders that added to another order's round trip.
void sample_reviews(State& s, const std::vector<const Lifecycle*>& lives, const std::set<OrderId>& shared,
                    std::int64_t& unfinished) {
  // A review to sample, created unfinished when missing; none once finished.
  const auto open_review = [&](SharedMap<std::string, TradeReview>& reviews, const std::string& key) -> TradeReview* {
    const auto found = reviews.find(key);
    if (found != reviews.end() && found->second.finished) return nullptr;
    if (found == reviews.end()) ++unfinished;
    return &reviews[key];
  };
  // Combined samples: the legs' P&L at the same instant, and the settlement
  // reference of a leg settled now as the underlying's price.
  const auto combined = [&](TradeReview& review, const std::vector<const Lifecycle*>& legs) {
    Money total;
    bool finished = true;
    std::optional<double> settled;
    for (const auto* leg : legs) {
      const auto value = pnl(s, *leg);
      if (!value) return;
      total = total + *value;
      finished = finished && leg->closed.has_value();
      if (!settled) settled = settled_spot(s, *leg);
    }
    sample(review, total, s.time, settled ? settled : spot(s, *legs.front()), finished);
  };
  std::map<OrderId, std::vector<const Lifecycle*>> strategies;
  std::map<std::string, std::vector<const Lifecycle*>> trades;
  for (const auto* life : lives) {
    if (const auto group = trade_group(*life, s.groups); unfinished_review(s.group_reviews, group) && s.group_reviews.contains(group))
      trades[group].push_back(life);
    // Older fills carry no review baseline. Do not invent historical extrema.
    if (!life->entry_context) continue;
    const auto* order = entry_order(s, *life);
    if (order && multi_leg(order->request)) strategies[order->id].push_back(life);
    auto* review = open_review(s.trade_reviews, std::to_string(life->first_fill));
    if (!review) continue;
    if (order && !multi_leg(order->request)) review->planned_risk = stop_risk(*life, *order);
    if (const auto value = pnl(s, *life)) sample(*review, *value, s.time, spot(s, *life), life->closed.has_value());
    if (review->finished) --unfinished;
  }
  for (const auto& [id, legs] : strategies) {
    const auto& order = s.orders[id - 1];
    // Each leg it opened started a round trip of its own, as a roll's new legs do;
    // contracts it shares with another order's round trip are not its alone.
    std::set<std::string> symbols;
    for (const auto* leg : legs) symbols.insert(leg->symbol);
    if (legs.size() < 2 || symbols.size() != legs.size() || shared.contains(id)) continue;
    auto* review = open_review(s.strategy_reviews, std::to_string(id));
    if (!review) continue;
    // The plan's stop when it has one, else what the structure can lose.
    review->planned_risk = combo_stop_risk(legs, order);
    if (!review->planned_risk) review->planned_risk = structure_risk(legs);
    if (!review->planned_risk) review->planned_risk = covered_debit(legs);
    combined(*review, legs);
    sample_trailing(s, *review, order, legs);
    if (review->finished) --unfinished;
  }
  for (const auto& [group, members] : trades) {
    auto& review = s.group_reviews[group];
    combined(review, members);
    if (review.finished) --unfinished;
  }
}
/// Every lifecycle the builder holds, in the order they opened: the open ones and
/// the finished ones it keeps.
std::vector<const Lifecycle*> held_lives(const Reviewing& r) {
  std::vector<std::pair<std::uint64_t, const Lifecycle*>> started;
  for (const auto& [symbol, open] : r.builder.open) started.emplace_back(open.started, &open.life);
  for (const auto& [number, life] : r.builder.closed) started.emplace_back(number, &life);
  std::sort(started.begin(), started.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<const Lifecycle*> lives;
  for (const auto& [number, life] : started) lives.push_back(life);
  return lives;
}
const Lifecycle* find_trip(const std::vector<const Lifecycle*>& lives, std::uint64_t id) {
  const auto it = std::find_if(lives.begin(), lives.end(), [&](const Lifecycle* life) { return life->first_fill == id; });
  return it == lives.end() ? nullptr : *it;
}
std::uint64_t trade_id(const std::string& text) {
  std::uint64_t id = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
  return error == std::errc{} && end == text.data() + text.size() ? id : 0;
}
/// The trade's review so far: its strategy's for a multi-leg first entry, else
/// its first round trip's.
const TradeReview* first_review(const State& s, const Lifecycle& root) {
  const auto* order = entry_order(s, root);
  const auto& reviews = order && multi_leg(order->request) ? s.strategy_reviews : s.trade_reviews;
  const auto found = reviews.find(std::to_string(order && multi_leg(order->request) ? order->id : root.first_fill));
  return found == reviews.end() ? nullptr : &found->second;
}
/// Joins the round trips the fills since the last call opened to the trade they
/// continue: the trade their order names (`group`), or for a multi-leg order the
/// trade of the earliest round trip of another order it reduced or added to at
/// that instant (a roll or an adjustment). Later round trips of an order follow its
/// first. A trade taking another entry starts its whole-trade review as its review
/// so far was, so the whole trade's extremes include those before the roll.
void assign_groups(State& s, std::int64_t& unfinished) {
  auto& effects = s.reviewing.builder.effects;
  if (effects.empty()) return;
  const auto lives = held_lives(s.reviewing);
  bool opened_any = false;
  for (std::size_t i = 0; i < effects.size();) {
    // One execution: an order's consecutive fills at one instant.
    const auto order_id = effects[i].order;
    const auto time = effects[i].time;
    std::vector<std::uint64_t> opened;
    std::optional<std::uint64_t> touched;
    for (; i < effects.size() && effects[i].order == order_id && effects[i].time == time; ++i) {
      const auto& e = effects[i];
      if (e.opened) opened.push_back(e.trade);
      else if (e.entry != order_id && (!touched || e.trade < *touched)) touched = e.trade;
    }
    if (opened.empty() || order_id == 0 || order_id > s.orders.size()) continue;
    opened_any = true;
    const auto& order = s.orders[order_id - 1];
    std::optional<std::string> target;
    const Lifecycle* continued = nullptr;
    if (!order.request.group.empty()) {
      continued = find_trip(lives, trade_id(order.request.group));
      target = continued ? trade_group(*continued, s.groups) : order.request.group;
    } else if (multi_leg(order.request) && touched) {
      continued = find_trip(lives, *touched);
      if (continued) target = trade_group(*continued, s.groups);
    }
    for (const auto id : opened) {
      const auto* life = find_trip(lives, id);
      if (!life) continue;
      auto group = target;
      if (!group && life->root != life->first_fill)
        if (const auto root = s.groups.find(std::to_string(life->root)); root != s.groups.end()) group = root->second;
      if (!group || *group == std::to_string(life->root)) continue;
      s.groups[std::to_string(id)] = *group;
    }
    if (!target || !continued || continued->entry_order == order_id || s.group_reviews.contains(*target)) continue;
    TradeReview review;
    if (const auto* root = find_trip(lives, trade_id(*target)))
      if (const auto* so_far = first_review(s, *root)) review = *so_far;
    review.finished = false;
    // Nothing recorded before: its extremes run from now.
    if (!review.worst) review.since = s.time;
    s.group_reviews[*target] = review;
    ++unfinished;
  }
  effects.clear();
  // Only executions that opened round trips move the mark, so other fills leave
  // the state as it was.
  if (opened_any) s.grouped = s.fills.size();
}
/// Applies the fills and closures recorded since the last call, interleaved as
/// lifecycles() interleaves them: a closure after the fills recorded before it.
void advance(State& s) {
  auto& r = s.reviewing;
  const auto closures_until = [&](std::uint64_t executed) {
    while (r.closures < s.closures.size() && s.closures[r.closures].after_fill <= executed)
      r.builder.closure(s.closures[r.closures++], s.contracts);
  };
  closures_until(r.fills);
  while (r.fills < s.fills.size()) {
    r.builder.fill(s.fills[r.fills], s.contracts);
    closures_until(++r.fills);
  }
}
/// Drops the finished lifecycles no review can use. One is kept while its trade
/// still holds a round trip, as a roll would start a whole-trade review from all
/// of them; while its own trade review is missing or unfinished; or while its
/// multi-leg entry's strategy review can still be sampled, as a rebuild would then
/// need all of that entry's legs: the review is under way, or the entry is still
/// open and can fill more. Orders that can no longer fill name no more roots.
void prune(State& s) {
  auto& r = s.reviewing;
  std::set<std::string> holding;
  for (const auto& [symbol, open] : r.builder.open) holding.insert(trade_group(open.life, s.groups));
  const auto entry = [&](const Lifecycle& life) -> const Order* {
    if (!life.entry_context) return nullptr;
    const auto* order = entry_order(s, life);
    return order && multi_leg(order->request) ? order : nullptr;
  };
  const auto live = [&](const Order& order) {
    const auto key = std::to_string(order.id);
    return unfinished_review(s.strategy_reviews, key) && (s.strategy_reviews.contains(key) || order.open());
  };
  std::erase_if(r.builder.closed, [&](const auto& numbered) {
    const auto& life = numbered.second;
    if (holding.contains(trade_group(life, s.groups))) return false;
    if (!life.entry_context) return true;
    if (unfinished_review(s.trade_reviews, std::to_string(life.first_fill))) return false;
    const auto* order = entry(life);
    return !order || !live(*order);
  });
  std::erase_if(r.builder.roots, [&](const auto& root) {
    return root.first == 0 || root.first > s.orders.size() || !s.orders[root.first - 1].open();
  });
}
std::size_t count_unfinished(const State& s) {
  std::size_t count = 0;
  for (const auto* reviews : {&s.trade_reviews, &s.strategy_reviews, &s.group_reviews})
    for (const auto& [key, review] : *reviews) count += review.finished ? 0 : 1;
  return count;
}
}  // namespace

void recount_reviews(State& s) {
  if (s.reviewing.ready) s.reviewing.unfinished = count_unfinished(s);
}

Decision regroup(State& s, const std::vector<std::uint64_t>& trades, bool together) {
  const auto refuse = [](Reason code, std::string message) { return Decision{code, std::move(message), {}, {}, {}}; };
  const auto all = lifecycles(s.fills, s.closures, s.contracts);
  std::vector<const Lifecycle*> listed;
  for (const auto id : trades) {
    auto it = std::find_if(all.begin(), all.end(), [&](const Lifecycle& life) { return life.first_fill == id; });
    if (it == all.end()) return refuse(Reason::UNKNOWN_TRADE, "No trade opens with fill " + std::to_string(id));
    if (it->closed && together) {
      // A closed round trip names its whole trade, when that still holds an open one.
      const auto trade = trade_group(*it, s.groups);
      const auto open = std::find_if(all.begin(), all.end(), [&](const Lifecycle& life) { return !life.closed && trade_group(life, s.groups) == trade; });
      if (open == all.end()) return refuse(Reason::INVALID_GROUP, "Trade " + std::to_string(id) + " is closed; only open trades join");
      it = open;
    }
    if (it->closed) return refuse(Reason::INVALID_GROUP, "Round trip " + std::to_string(id) + " is closed; only open round trips leave a trade");
    if (!listed.empty() && it->contract.underlying != listed.front()->contract.underlying)
      return refuse(Reason::INVALID_GROUP, "A trade's round trips share one underlying");
    if (std::find(listed.begin(), listed.end(), &*it) == listed.end()) listed.push_back(&*it);
  }
  if (listed.empty() || (together && listed.size() < 2))
    return refuse(Reason::INVALID_GROUP, together ? "Name at least two open round trips to group" : "Name an open round trip to ungroup");
  const auto set_group = [&](const Lifecycle& life, const std::string& group) {
    const auto key = std::to_string(life.first_fill);
    if (group == std::to_string(life.root)) s.groups.erase(key);
    else s.groups[key] = group;
  };
  const auto planned_risk = [&](const std::string& group) -> std::optional<Money> {
    if (const auto found = s.group_reviews.find(group); found != s.group_reviews.end()) return found->second.planned_risk;
    for (const auto& life : all)
      if (life.first_fill == trade_id(group))
        if (const auto* review = first_review(s, life)) return review->planned_risk;
    return {};
  };
  // Each trade's whole-trade review restarts now, while it holds more than one entry.
  const auto restart = [&](const std::string& group, const std::vector<const Lifecycle*>& members, std::optional<Money> previous_risk) {
    std::set<OrderId> entries;
    std::vector<const Lifecycle*> open;
    for (const auto* life : members) {
      entries.insert(life->entry_order);
      if (!life->closed) open.push_back(life);
    }
    if (entries.size() < 2) return;
    TradeReview review;
    review.since = s.time;
    if (!open.empty()) {
      review.planned_risk = structure_risk(open);
      if (!review.planned_risk) review.planned_risk = covered_debit(open);
    }
    if (!review.planned_risk) review.planned_risk = previous_risk;
    s.group_reviews[group] = review;
  };
  std::map<std::string, std::vector<const Lifecycle*>> members;
  for (const auto* life : listed) members[trade_group(*life, s.groups)];
  for (const auto& life : all)
    if (const auto group = trade_group(life, s.groups); members.contains(group)) members[group].push_back(&life);
  if (together) {
    if (members.size() == 1) return {};
    std::string target = members.begin()->first;
    for (const auto& [group, lives] : members) if (trade_id(group) < trade_id(target)) target = group;
    const auto previous_risk = planned_risk(target);
    std::vector<const Lifecycle*> joined;
    for (const auto& [group, lives] : members) {
      s.group_reviews.erase(group);
      for (const auto* life : lives) { set_group(*life, target); joined.push_back(life); }
    }
    restart(target, joined, previous_risk);
  } else {
    std::set<std::uint64_t> leaving;
    for (const auto* life : listed) leaving.insert(life->first_fill);
    for (const auto& [group, lives] : members) {
      if (lives.size() < 2) continue;
      const auto previous_risk = planned_risk(group);
      s.group_reviews.erase(group);
      std::vector<const Lifecycle*> staying;
      for (const auto* life : lives) {
        if (leaving.contains(life->first_fill)) set_group(*life, std::to_string(life->first_fill));
        else staying.push_back(life);
      }
      if (staying.empty()) continue;
      // A trade named by a round trip that left takes the oldest that stayed.
      auto name = group;
      if (leaving.contains(trade_id(group))) {
        name = std::to_string(staying.front()->first_fill);
        for (const auto* life : staying) if (life->first_fill < trade_id(name)) name = std::to_string(life->first_fill);
      }
      for (const auto* life : staying) set_group(*life, name);
      restart(name, staying, previous_risk);
    }
  }
  recount_reviews(s);
  return {};
}

void update_reviews_rebuilt(State& s) {
  const auto done = [](const auto& entry) { return entry.second.finished; };
  if (s.ledger.positions().empty() && std::all_of(s.trade_reviews.begin(), s.trade_reviews.end(), done) &&
      std::all_of(s.strategy_reviews.begin(), s.strategy_reviews.end(), done) &&
      std::all_of(s.group_reviews.begin(), s.group_reviews.end(), done))
    return;
  std::set<OrderId> shared;
  const auto rebuilt = lifecycles(s.fills, s.closures, s.contracts, &shared);
  std::vector<const Lifecycle*> lives;
  for (const auto& life : rebuilt) lives.push_back(&life);
  std::int64_t unfinished = 0;
  sample_reviews(s, lives, shared, unfinished);
}

void update_reviews(State& s) {
  auto& r = s.reviewing;
  std::int64_t unfinished = 0;
  if (!r.ready) {
    // After a load: one pass over the history, then only what can change. The
    // fills up to `grouped` joined their trades when they were recorded.
    r = Reviewing{};
    r.ready = true;
    r.unfinished = count_unfinished(s);
    r.builder.record = true;
    r.builder.record_from = s.grouped;
    advance(s);
    assign_groups(s, unfinished);
    prune(s);
  } else {
    advance(s);
    assign_groups(s, unfinished);
  }
  std::optional<State> expected;
  if (verify_reviews) {
    expected = s;
    update_reviews_rebuilt(*expected);
  }
  // A flat account whose reviews are all finished has nothing to sample.
  if (!s.ledger.positions().empty() || static_cast<std::int64_t>(r.unfinished) + unfinished > 0) {
    // Every lifecycle a rebuild would sample: the open ones, and the finished ones
    // a review still needs.
    sample_reviews(s, held_lives(r), r.builder.shared, unfinished);
    prune(s);
  }
  r.unfinished = static_cast<std::size_t>(static_cast<std::int64_t>(r.unfinished) + unfinished);
  if (expected) {
    if (Json(s.trade_reviews) != Json(expected->trade_reviews) || Json(s.strategy_reviews) != Json(expected->strategy_reviews) ||
        Json(s.group_reviews) != Json(expected->group_reviews))
      throw std::logic_error("update_reviews differs from a rebuild from every fill");
    if (r.unfinished != count_unfinished(s)) throw std::logic_error("update_reviews miscounted unfinished reviews");
  }
  s.settling.clear();
}
}  // namespace openport::trading::detail

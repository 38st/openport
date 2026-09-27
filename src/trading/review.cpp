#include "review.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

#include "openport/trading/history.hpp"

namespace openport::trading::detail {
namespace {
std::optional<double> spot(const State& s, const Lifecycle& life) {
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
const Order* entry_order(const State& s, const Lifecycle& life) {
  if (life.first_fill == 0 || life.first_fill > s.fills.size()) return nullptr;
  const auto id = s.fills[life.first_fill - 1].order_id;
  return id > 0 && id <= s.orders.size() ? &s.orders[id - 1] : nullptr;
}
std::optional<Money> stop_risk(const Lifecycle& life, const Order& order) {
  if (!order.request.bracket || !order.request.bracket->stop_loss) return {};
  const auto& stop = order.request.bracket->stop_loss->trigger;
  // An underlying level does not specify the option's execution price.
  if (!stop || stop->source != TriggerSource::Option) return {};
  const Money difference = life.direction > 0
      ? life.open_notional - stop->level * life.opened_contracts
      : stop->level * life.opened_contracts - life.open_notional;
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
/// Samples the reviews of `lives`, given in the order they opened, and of the
/// multi-leg entries among them. A finished review stays as it is; a missing one
/// starts now. `unfinished` changes by the change in unfinished reviews.
void sample_reviews(State& s, const std::vector<const Lifecycle*>& lives, std::int64_t& unfinished) {
  // A review to sample, created unfinished when missing; none once finished.
  const auto open_review = [&](SharedMap<std::string, TradeReview>& reviews, const std::string& key) -> TradeReview* {
    const auto found = reviews.find(key);
    if (found != reviews.end() && found->second.finished) return nullptr;
    if (found == reviews.end()) ++unfinished;
    return &reviews[key];
  };
  std::map<OrderId, std::vector<const Lifecycle*>> strategies;
  for (const auto* life : lives) {
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
    if (legs.size() != order.request.legs.size()) continue;
    auto* review = open_review(s.strategy_reviews, std::to_string(id));
    if (!review) continue;
    review->planned_risk = structure_risk(legs);
    Money total;
    bool complete = true, finished = true;
    for (const auto* leg : legs) {
      const auto value = pnl(s, *leg);
      if (!value) { complete = false; break; }
      total = total + *value;
      finished = finished && leg->closed.has_value();
    }
    if (complete) sample(*review, total, s.time, spot(s, *legs.front()), finished);
    if (review->finished) --unfinished;
  }
}
bool unfinished_review(const SharedMap<std::string, TradeReview>& reviews, const std::string& key) {
  const auto found = reviews.find(key);
  return found == reviews.end() || !found->second.finished;
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
/// Drops the finished lifecycles no review can use. One is kept while its own
/// trade review is missing or unfinished, or while its multi-leg entry's
/// strategy review can still be sampled, as a rebuild would then need all of
/// that entry's legs: they match its legs now, or the entry is still open and
/// can fill more. A strategy that cannot be sampled never can again.
void prune(State& s) {
  auto& r = s.reviewing;
  const auto entry = [&](const Lifecycle& life) -> const Order* {
    if (!life.entry_context) return nullptr;
    const auto* order = entry_order(s, life);
    return order && multi_leg(order->request) ? order : nullptr;
  };
  std::map<OrderId, std::size_t> legs;
  for (const auto& [symbol, open] : r.builder.open) if (const auto* order = entry(open.life)) ++legs[order->id];
  for (const auto& [number, life] : r.builder.closed) if (const auto* order = entry(life)) ++legs[order->id];
  const auto live = [&](const Order& order) {
    if (!unfinished_review(s.strategy_reviews, std::to_string(order.id))) return false;
    const auto count = legs[order.id];
    const auto wanted = order.request.legs.size();
    return count == wanted || (count < wanted && order.open());
  };
  std::erase_if(r.builder.closed, [&](const auto& numbered) {
    const auto& life = numbered.second;
    if (!life.entry_context) return true;
    if (unfinished_review(s.trade_reviews, std::to_string(life.first_fill))) return false;
    const auto* order = entry(life);
    return !order || !live(*order);
  });
}
std::size_t count_unfinished(const State& s) {
  std::size_t count = 0;
  for (const auto& [key, review] : s.trade_reviews) count += review.finished ? 0 : 1;
  for (const auto& [key, review] : s.strategy_reviews) count += review.finished ? 0 : 1;
  return count;
}
}  // namespace

void update_reviews_rebuilt(State& s) {
  const auto done = [](const auto& entry) { return entry.second.finished; };
  if (s.ledger.positions().empty() && std::all_of(s.trade_reviews.begin(), s.trade_reviews.end(), done) &&
      std::all_of(s.strategy_reviews.begin(), s.strategy_reviews.end(), done))
    return;
  const auto rebuilt = lifecycles(s.fills, s.closures, s.contracts);
  std::vector<const Lifecycle*> lives;
  for (const auto& life : rebuilt) lives.push_back(&life);
  std::int64_t unfinished = 0;
  sample_reviews(s, lives, unfinished);
}

void update_reviews(State& s) {
  std::optional<State> expected;
  if (verify_reviews) {
    expected = s;
    update_reviews_rebuilt(*expected);
  }
  auto& r = s.reviewing;
  if (!r.ready) {
    // After a load: one pass over the history, then only what can change.
    r = Reviewing{};
    r.ready = true;
    r.unfinished = count_unfinished(s);
    advance(s);
    prune(s);
  } else {
    advance(s);
  }
  // A flat account whose reviews are all finished has nothing to sample.
  if (!s.ledger.positions().empty() || r.unfinished > 0) {
    // Every lifecycle a rebuild would sample, in the order they opened: the open
    // ones, and the finished ones a review still needs.
    std::vector<std::pair<std::uint64_t, const Lifecycle*>> started;
    for (const auto& [symbol, open] : r.builder.open) started.emplace_back(open.started, &open.life);
    for (const auto& [number, life] : r.builder.closed) started.emplace_back(number, &life);
    std::sort(started.begin(), started.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<const Lifecycle*> lives;
    for (const auto& [number, life] : started) lives.push_back(life);
    std::int64_t unfinished = 0;
    sample_reviews(s, lives, unfinished);
    r.unfinished = static_cast<std::size_t>(static_cast<std::int64_t>(r.unfinished) + unfinished);
    prune(s);
  }
  if (expected) {
    if (Json(s.trade_reviews) != Json(expected->trade_reviews) || Json(s.strategy_reviews) != Json(expected->strategy_reviews))
      throw std::logic_error("update_reviews differs from a rebuild from every fill");
    if (r.unfinished != count_unfinished(s)) throw std::logic_error("update_reviews miscounted unfinished reviews");
  }
}
}  // namespace openport::trading::detail

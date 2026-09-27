#include "review.hpp"

#include <algorithm>
#include <cmath>

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
}  // namespace

void update_reviews(State& s) {
  // A flat account whose reviews are all finished has nothing to sample, so the
  // lifecycles are not rebuilt from every fill on each market batch.
  const auto done = [](const auto& entry) { return entry.second.finished; };
  if (s.ledger.positions().empty() && std::all_of(s.trade_reviews.begin(), s.trade_reviews.end(), done) &&
      std::all_of(s.strategy_reviews.begin(), s.strategy_reviews.end(), done))
    return;
  const auto lives = lifecycles(s.fills, s.closures, s.contracts);
  std::map<OrderId, std::vector<const Lifecycle*>> strategies;
  for (const auto& life : lives) {
    // Older fills carry no review baseline. Do not invent historical extrema.
    if (!life.entry_context) continue;
    const auto* order = entry_order(s, life);
    if (order && multi_leg(order->request)) strategies[order->id].push_back(&life);
    const auto key = std::to_string(life.first_fill);
    auto& review = s.trade_reviews[key];
    if (review.finished) continue;
    if (order && !multi_leg(order->request)) review.planned_risk = stop_risk(life, *order);
    if (const auto value = pnl(s, life)) sample(review, *value, s.time, spot(s, life), life.closed.has_value());
  }
  for (const auto& [id, legs] : strategies) {
    const auto& order = s.orders[id - 1];
    if (legs.size() != order.request.legs.size()) continue;
    auto& review = s.strategy_reviews[std::to_string(id)];
    if (review.finished) continue;
    review.planned_risk = structure_risk(legs);
    Money total;
    bool complete = true, finished = true;
    for (const auto* leg : legs) {
      const auto value = pnl(s, *leg);
      if (!value) { complete = false; break; }
      total = total + *value;
      finished = finished && leg->closed.has_value();
    }
    if (complete) sample(review, total, s.time, spot(s, *legs.front()), finished);
  }
}
}  // namespace openport::trading::detail

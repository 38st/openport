#include "openport/trading/evaluation.hpp"

#include <algorithm>
#include <random>

namespace openport::trading {
Money evaluation_floor(const AccountRules& rules, Money peak, bool& locked) {
  if (rules.max_drawdown <= Money{}) return {};
  if (rules.lock_balance > Money{} && (locked || peak - rules.max_drawdown >= rules.lock_balance)) {
    locked = true;
    return rules.lock_balance;
  }
  return peak - rules.max_drawdown;
}
EvaluationStatus evaluate_equity(Evaluation& evaluation, const AccountRules& rules, Money equity) {
  if (!rules.evaluation() || evaluation.status != EvaluationStatus::Active) return evaluation.status;
  if (rules.drawdown_mode == DrawdownMode::Intraday && equity > evaluation.peak) {
    evaluation.peak = equity;
    evaluation.floor = evaluation_floor(rules, evaluation.peak, evaluation.floor_locked);
  }
  if (rules.max_drawdown > Money{} && equity <= evaluation.floor) return EvaluationStatus::Failed;
  if (rules.profit_target > Money{} && equity >= evaluation.starting_balance + rules.profit_target)
    return EvaluationStatus::Passed;
  return EvaluationStatus::Active;
}
void evaluation_rollover(Evaluation& evaluation, const AccountRules& rules) {
  if (rules.evaluation() && evaluation.status == EvaluationStatus::Active &&
      rules.drawdown_mode == DrawdownMode::EndOfDay && evaluation.day_close_equity > evaluation.peak) {
    evaluation.peak = evaluation.day_close_equity;
    evaluation.floor = evaluation_floor(rules, evaluation.peak, evaluation.floor_locked);
  }
}
PassOdds pass_odds(const Evaluation& current, const AccountRules& rules, Money equity,
    const std::vector<EvaluationDay>& history, int days, int samples, std::uint64_t seed) {
  if (days < 1 || days > 252 || samples < 1 || samples > 10000)
    throw std::invalid_argument("days must be 1–252 and samples 1–10000");
  if (!rules.evaluation()) throw std::invalid_argument("This account has no evaluation target or drawdown rule");
  // The day's open and close are points on its path too. The recorded extremes
  // start from the day's first fully marked observation, so a day that gapped and
  // kept going has a low above its open or a high below it: widen them to cover
  // the open and close rather than drop the trending days that decide attempts.
  struct Day { Money open, low, high, close; bool low_first; };
  std::vector<Day> usable;
  for (const auto& day : history) {
    if (!day.low_equity || !day.high_equity || day.low_at <= 0 || day.high_at <= 0) continue;
    usable.push_back({day.open_equity, std::min({*day.low_equity, day.open_equity, day.close_equity}),
        std::max({*day.high_equity, day.open_equity, day.close_equity}), day.close_equity, day.low_at <= day.high_at});
  }
  if (usable.size() < 10) throw std::invalid_argument("Pass odds need at least 10 completed days with recorded intraday lows and highs");
  PassOdds result;
  result.historical_days = usable.size();
  result.seed = seed;
  std::mt19937_64 random(seed);
  std::vector<int> passed;
  int failed = 0;
  auto initial = current;
  initial.days.clear();
  initial.payouts.clear();
  for (int sample = 0; sample < samples; ++sample) {
    auto evaluation = initial;
    auto balance = equity;
    // Start the first projected day after closing the current day.
    evaluation.day_close_equity = balance;
    evaluation_rollover(evaluation, rules);
    std::size_t index = 0;
    int elapsed = 0;
    for (; elapsed < days && evaluation.status == EvaluationStatus::Active; ++elapsed) {
      if (elapsed % 3 == 0) index = static_cast<std::size_t>(random() % usable.size());
      const auto& day = usable[index];
      index = (index + 1) % usable.size();
      const auto observe = [&](Money historical) {
        if (evaluation.status == EvaluationStatus::Active)
          evaluation.status = evaluate_equity(evaluation, rules, balance + historical - day.open);
      };
      observe(day.open);
      observe(day.low_first ? day.low : day.high);
      observe(day.low_first ? day.high : day.low);
      observe(day.close);
      balance = balance + day.close - day.open;
      evaluation.day_close_equity = balance;
      evaluation_rollover(evaluation, rules);
    }
    if (evaluation.status == EvaluationStatus::Passed) passed.push_back(elapsed);
    if (evaluation.status == EvaluationStatus::Failed) ++failed;
  }
  result.pass = static_cast<double>(passed.size()) / samples;
  result.fail = static_cast<double>(failed) / samples;
  result.neither = static_cast<double>(samples - static_cast<int>(passed.size()) - failed) / samples;
  if (!passed.empty()) {
    std::sort(passed.begin(), passed.end());
    result.median_days_to_pass = (passed[(passed.size() - 1) / 2] + passed[passed.size() / 2]) / 2.0;
  }
  return result;
}
}  // namespace openport::trading

#include "openport/trading/evaluation.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>

#include "openport/md/time.hpp"

namespace openport::trading {
namespace {
__extension__ using Wide = __int128;
std::string dollars(Money value) { return (value < Money{} ? "-$" + (-value).str() : "$" + value.str()); }
std::string days_text(std::uint64_t n) { return std::to_string(n) + (n == 1 ? " day" : " days"); }
/// A peak that follows every high (intraday and static plans) moves with fully
/// marked equity; an end-of-day one only at rollover.
void ratchet(Evaluation& evaluation, const AccountRules& rules, Money equity) {
  if (rules.drawdown_mode != DrawdownMode::EndOfDay && equity > evaluation.peak) {
    evaluation.peak = equity;
    evaluation.floor = evaluation_floor(rules, evaluation.peak, evaluation.floor_locked, evaluation.starting_balance);
  }
}
/// Adds one day to the counts the objectives read.
void add_day(DayStats& stats, const AccountRules& rules, md::Date day, Money profit, std::uint64_t executions) {
  ++stats.days;
  if (executions > 0) ++stats.trading_days;
  if (profit <= Money{}) return;
  if (profit >= rules.profitable_day_profit) ++stats.profitable_days;
  stats.positive_total = stats.positive_total + profit;
  if (!stats.best_day || profit > *stats.best_day) {
    stats.best_day = profit;
    stats.best_day_date = day;
  }
}
DayStats finished_day_stats(const Evaluation& evaluation, const AccountRules& rules) {
  DayStats stats;
  for (const auto& day : evaluation.days) add_day(stats, rules, day.day, day_profit(day, rules), day.executions);
  return stats;
}
/// day_stats from the finished days' counts and the day in progress.
DayStats with_today(DayStats stats, const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  if (evaluation.started > 0) add_day(stats, rules, evaluation.day, today_profit(evaluation, rules, now), evaluation.day_executions);
  return stats;
}
/// The consistency rule's basis: the attempt's profit, or its profitable days' sum.
Money consistency_basis(const AccountRules& rules, const DayStats& stats, Money profit) {
  return rules.consistency_basis == ConsistencyBasis::PositiveDays ? stats.positive_total : profit;
}
/// best * 100 <= percent * basis, exactly.
bool consistent(Money best, std::int64_t percent, Money basis) {
  return static_cast<Wide>(best.micros()) * 100 <= static_cast<Wide>(basis.micros()) * percent;
}
std::string basis_name(DailyLossBasis basis) {
  switch (basis) {
    case DailyLossBasis::Equity: return "the day's opening equity";
    case DailyLossBasis::Balance: return "the day's opening balance";
    case DailyLossBasis::Higher: return "the higher of the day's opening balance and equity";
    case DailyLossBasis::Peak: return "the day's equity high";
  }
  return "the day's opening equity";
}
bool needs_days(const AccountRules& rules) {
  return rules.min_trading_days > 0 || rules.min_profitable_days > 0 || rules.consistency_percent > 0;
}
std::vector<Objective> objectives_with(const Evaluation& e, const AccountRules& rules, const PlanInputs& now, const DayStats* finished) {
  std::vector<Objective> out;
  const auto profit = attempt_profit(e, rules, now);
  const bool balance = rules.profit_basis == ProfitBasis::Balance;
  if (rules.profit_target > Money{}) {
    Objective o{Reason::PROFIT_TARGET, profit >= rules.profit_target && (!balance || now.flat),
                profit.dollars(), rules.profit_target.dollars(), {}};
    const auto target = e.starting_balance + rules.profit_target;
    const auto measured = balance ? "The closed balance " + dollars(now.balance) : "Equity " + dollars(now.equity);
    if (o.met) o.message = measured + " is at or above the target " + dollars(target);
    else if (profit >= rules.profit_target) o.message = measured + " reached the target " + dollars(target) + "; close every position to count it";
    else o.message = dollars(rules.profit_target - profit) + " to go to the target " + dollars(target) +
                     (balance ? " on the closed balance" : "");
    out.push_back(std::move(o));
  }
  if (!needs_days(rules)) return out;
  const auto stats = with_today(finished ? *finished : finished_day_stats(e, rules), e, rules, now);
  if (rules.min_trading_days > 0) {
    const auto required = static_cast<std::uint64_t>(rules.min_trading_days);
    out.push_back({Reason::MIN_TRADING_DAYS, stats.trading_days >= required, static_cast<double>(stats.trading_days),
                   static_cast<double>(required), std::to_string(stats.trading_days) + " of " + days_text(required) +
                   " with a trade; a day counts once your own order executes on it"});
  }
  if (rules.min_profitable_days > 0) {
    const auto required = static_cast<std::uint64_t>(rules.min_profitable_days);
    out.push_back({Reason::MIN_PROFITABLE_DAYS, stats.profitable_days >= required, static_cast<double>(stats.profitable_days),
                   static_cast<double>(required), std::to_string(stats.profitable_days) + " of " + days_text(required) +
                   " with a profit" + (rules.profitable_day_profit > Money{} ? " of at least " + dollars(rules.profitable_day_profit) : "")});
  }
  if (rules.consistency_percent > 0) {
    const auto basis = consistency_basis(rules, stats, profit);
    Objective o{Reason::CONSISTENCY, true, std::nullopt, static_cast<double>(rules.consistency_percent), {}};
    const std::string of = rules.consistency_basis == ConsistencyBasis::PositiveDays ? "the profitable days' total " : "the total profit ";
    if (!stats.best_day) {
      o.message = "No profitable day yet";
    } else {
      o.met = consistent(*stats.best_day, rules.consistency_percent, basis);
      if (basis > Money{}) o.actual = stats.best_day->dollars() / basis.dollars() * 100;
      o.message = "The best day, " + dollars(*stats.best_day) + " on " + md::format_date(stats.best_day_date) + ", is " +
          (basis > Money{} ? std::to_string(static_cast<int>(*o.actual + 0.5)) + "% of " + of + dollars(basis)
                           : "all of " + of + dollars(basis)) +
          "; at most " + std::to_string(rules.consistency_percent) + "% may come from one day";
    }
    out.push_back(std::move(o));
  }
  return out;
}
}  // namespace
Money lock_level(const AccountRules& rules, Money starting_balance) {
  return rules.lock_at_start ? starting_balance : rules.lock_balance;
}
Money evaluation_floor(const AccountRules& rules, Money peak, bool& locked, Money starting_balance) {
  if (rules.max_drawdown <= Money{}) return {};
  if (rules.drawdown_mode == DrawdownMode::Static) {
    locked = true;
    return starting_balance - rules.max_drawdown;
  }
  const auto lock = lock_level(rules, starting_balance);
  if (lock > Money{} && (locked || peak - rules.max_drawdown >= lock)) {
    locked = true;
    return lock;
  }
  return peak - rules.max_drawdown;
}
EvaluationStatus evaluate_equity(Evaluation& evaluation, const AccountRules& rules, Money equity) {
  if (evaluation.status != EvaluationStatus::Active) return evaluation.status;
  // An account without rules keeps its high-water mark too; only rules decide.
  ratchet(evaluation, rules, equity);
  if (!rules.evaluation()) return EvaluationStatus::Active;
  if (rules.max_drawdown > Money{} && equity <= evaluation.floor) return EvaluationStatus::Failed;
  if (rules.profit_target > Money{} && equity >= evaluation.starting_balance + rules.profit_target)
    return EvaluationStatus::Passed;
  return EvaluationStatus::Active;
}
void evaluation_rollover(Evaluation& evaluation, const AccountRules& rules) {
  if (evaluation.status == EvaluationStatus::Active && rules.drawdown_mode == DrawdownMode::EndOfDay &&
      evaluation.day_close_equity > evaluation.peak) {
    evaluation.peak = evaluation.day_close_equity;
    evaluation.floor = evaluation_floor(rules, evaluation.peak, evaluation.floor_locked, evaluation.starting_balance);
  }
}
Money evaluation_tomorrow_floor(const Evaluation& evaluation, const AccountRules& rules, Money equity) {
  if (evaluation.status != EvaluationStatus::Active || rules.drawdown_mode != DrawdownMode::EndOfDay ||
      equity <= evaluation.peak)
    return evaluation.floor;
  bool locked = evaluation.floor_locked;
  return evaluation_floor(rules, equity, locked, evaluation.starting_balance);
}
md::Date plan_trading_date(const AccountRules& rules, Timestamp time) {
  return md::trading_date(time, static_cast<int>(rules.day_end_minutes));
}
bool counts_executions(const AccountRules& rules) { return rules.min_trading_days > 0; }
Money day_profit(const EvaluationDay& day, const AccountRules& rules) {
  return rules.profit_basis == ProfitBasis::Balance ? day.realised : day.close_equity - day.open_equity;
}
Money today_profit(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  return rules.profit_basis == ProfitBasis::Balance ? now.net_realised - evaluation.day_open_realised
                                                    : now.equity - evaluation.day_open_equity;
}
Money attempt_profit(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  Money withdrawn;
  for (const auto& payout : evaluation.payouts) withdrawn = withdrawn + payout.amount;
  return (rules.profit_basis == ProfitBasis::Balance ? now.balance : now.equity) + withdrawn - evaluation.starting_balance;
}
Money day_open_balance(const Evaluation& evaluation, const PlanInputs& now) {
  return now.balance - (now.net_realised - evaluation.day_open_realised);
}
DayStats day_stats(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  return with_today(finished_day_stats(evaluation, rules), evaluation, rules, now);
}
std::vector<Objective> evaluation_objectives(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  return objectives_with(evaluation, rules, now, nullptr);
}
std::optional<Money> consistency_target(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  if (rules.consistency_percent <= 0) return std::nullopt;
  const auto stats = day_stats(evaluation, rules, now);
  if (!stats.best_day) return std::nullopt;
  // Whole cents, rounded up, so the amount always satisfies the rule.
  const Wide scaled = static_cast<Wide>(stats.best_day->micros()) * 100;
  const Wide micros = (scaled + rules.consistency_percent - 1) / rules.consistency_percent;
  const Wide cents = (micros + 9'999) / 10'000;
  return Money::from_micros(static_cast<std::int64_t>(cents * 10'000));
}
std::optional<DailyLossLevel> daily_loss_level(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  if (rules.daily_loss_limit <= Money{}) return std::nullopt;
  Money reference = evaluation.day_open_equity;
  switch (rules.daily_loss_basis) {
    case DailyLossBasis::Equity: break;
    case DailyLossBasis::Balance: reference = day_open_balance(evaluation, now); break;
    case DailyLossBasis::Higher: reference = std::max(reference, day_open_balance(evaluation, now)); break;
    case DailyLossBasis::Peak: reference = std::max(reference, evaluation.day_high_equity.value_or(reference)); break;
  }
  return DailyLossLevel{reference, reference - rules.daily_loss_limit};
}
namespace {
PlanVerdict evaluate_with(Evaluation& e, const AccountRules& rules, const PlanInputs& now, const DayStats* finished) {
  PlanVerdict verdict;
  if (e.status != EvaluationStatus::Active) return verdict;
  ratchet(e, rules, now.equity);
  if (!rules.evaluation()) return verdict;
  if (rules.max_drawdown > Money{} && now.equity <= e.floor) {
    verdict.status = EvaluationStatus::Failed;
    verdict.code = Reason::DRAWDOWN_FLOOR;
    verdict.level = e.floor;
    verdict.message = "Equity " + dollars(now.equity) + " reached the drawdown floor " + dollars(e.floor) + " (peak " +
                      dollars(e.peak) + ", max drawdown " + dollars(rules.max_drawdown) + ")";
    return verdict;
  }
  if (e.day_lock == Reason::NONE)
    if (const auto daily = daily_loss_level(e, rules, now); daily && now.equity <= daily->level) {
      verdict.code = Reason::DAILY_LOSS_LIMIT;
      verdict.level = daily->level;
      verdict.message = "Equity " + dollars(now.equity) + " reached the daily loss limit at " + dollars(daily->level) + " (" +
                        dollars(rules.daily_loss_limit) + " below " + basis_name(rules.daily_loss_basis) + ", " +
                        dollars(daily->reference) + ")";
      if (rules.daily_loss_action == BreachAction::Fail) {
        verdict.status = EvaluationStatus::Failed;
      } else {
        verdict.lock = true;
        verdict.message += ": positions are closed and opening orders refused until the next trading day";
      }
      return verdict;
    }
  if (rules.profit_target <= Money{}) return verdict;
  const auto objectives = objectives_with(e, rules, now, finished);
  if (!std::all_of(objectives.begin(), objectives.end(), [](const Objective& o) { return o.met; })) return verdict;
  const auto target = e.starting_balance + rules.profit_target;
  verdict.status = EvaluationStatus::Passed;
  verdict.code = Reason::PROFIT_TARGET;
  verdict.level = target;
  verdict.message = rules.profit_basis == ProfitBasis::Balance
      ? "Balance " + dollars(now.balance) + " reached the profit target " + dollars(target) + " with every position closed"
      : "Equity " + dollars(now.equity) + " reached the profit target " + dollars(target);
  return verdict;
}
}  // namespace
PlanVerdict evaluate_plan(Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  return evaluate_with(evaluation, rules, now, nullptr);
}
std::string day_lock_message(Reason lock) {
  if (lock == Reason::DAILY_LOSS_LIMIT)
    return "The plan's daily loss limit locked the account until the next trading day; only closing orders are accepted";
  return "The plan locked the account until the next trading day; only closing orders are accepted";
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
  // The attempt's finished days count toward its objectives; the projection then
  // treats each simulated path as both equity and closed balance, flat at every
  // observation, and assumes a trade on every simulated day.
  auto initial = current;
  initial.payouts.clear();
  const auto counted = needs_days(rules);
  const auto close_day = [&](Evaluation& e, DayStats& stats, Money close) {
    e.day_close_equity = close;
    if (counted) {
      EvaluationDay day;
      day.day = e.day;
      day.open_equity = e.day_open_equity;
      day.close_equity = close;
      day.realised = close - e.day_open_equity;
      day.executions = e.day_executions;
      add_day(stats, rules, day.day, day_profit(day, rules), day.executions);
    }
    evaluation_rollover(e, rules);
  };
  const auto finished = counted ? finished_day_stats(initial, rules) : DayStats{};
  for (int sample = 0; sample < samples; ++sample) {
    auto evaluation = initial;
    auto stats = finished;
    auto balance = equity;
    // Start the first projected day after closing the current day, whose profit so
    // far counts on the equity basis.
    evaluation.day_open_realised = Money{};
    close_day(evaluation, stats, balance);
    std::size_t index = 0;
    int elapsed = 0;
    for (; elapsed < days && evaluation.status == EvaluationStatus::Active; ++elapsed) {
      if (elapsed % 3 == 0) index = static_cast<std::size_t>(random() % usable.size());
      const auto& day = usable[index];
      index = (index + 1) % usable.size();
      evaluation.day = md::date_from_days(md::days_since_epoch(evaluation.day) + 1);
      evaluation.day_open_equity = balance;
      evaluation.day_high_equity = balance;
      evaluation.day_lock = Reason::NONE;
      evaluation.day_executions = 1;
      auto close = balance + day.close - day.open;
      const auto observe = [&](Money historical) {
        if (evaluation.status != EvaluationStatus::Active || evaluation.day_lock != Reason::NONE) return;
        const auto value = balance + historical - day.open;
        evaluation.day_high_equity = std::max(*evaluation.day_high_equity, value);
        const auto verdict = evaluate_with(evaluation, rules, {value, value, value - evaluation.day_open_equity, true}, &stats);
        if (verdict.decided()) evaluation.status = verdict.status;
        // A lock closes the account at its level for the rest of the day.
        if (verdict.lock) { evaluation.day_lock = verdict.code; close = verdict.level; }
      };
      observe(day.open);
      observe(day.low_first ? day.low : day.high);
      observe(day.low_first ? day.high : day.low);
      observe(day.close);
      balance = close;
      close_day(evaluation, stats, balance);
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

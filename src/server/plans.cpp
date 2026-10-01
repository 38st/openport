#include "openport/server/plans.hpp"

#include <algorithm>
#include <array>

namespace openport::server {
namespace {
using trading::AccountRules;
using trading::DrawdownMode;
using trading::Money;

PlanPreset evaluation(std::string_view style, std::int64_t thousands) {
  const bool intraday = style == "intraday";
  const auto size = std::to_string(thousands) + "K";
  const auto cash = Money::from_micros(thousands * 1'000'000'000);
  AccountRules rules;
  rules.plan = (intraday ? "Intraday " : "End-of-day ") + size;
  // Exact whole-dollar percentages of whole-thousand balances.
  rules.profit_target = cash.prorate(intraday ? 10 : 12, 100);
  rules.max_drawdown = cash.prorate(intraday ? 5 : 6, 100);
  rules.drawdown_mode = intraday ? DrawdownMode::Intraday : DrawdownMode::EndOfDay;
  rules.buy_only = intraday;
  rules.buying_power = true;
  rules.slippage_ticks = 0;
  rules.margin = trading::MarginMode::Strategy;
  rules.expiry_cutoff = 5 * md::kNanosPerMinute;
  return {std::string(style) + "-" + std::to_string(thousands) + "k", rules.plan,
          intraday ? "Buy-only single-leg options. 10% profit target; 5% trailing drawdown that rises with every new equity high."
                   : "Any strategy. 12% profit target; 6% trailing drawdown that rises only with each day's closing equity.",
          cash, rules, {}};
}
/// Evaluations built on the plan objectives: `static` keeps a static floor,
/// counts its target on the closed balance, fails a day that loses 4% of its opening
/// balance and needs four trading days, on a day that ends at 6 pm ET; `locking`
/// trails each close until the floor reaches the starting balance, locks a day that
/// loses 2% and lets no day make more than half the profit.
PlanPreset objective_evaluation(std::string_view style, std::int64_t thousands) {
  const bool fixed = style == "static";
  const auto size = std::to_string(thousands) + "K";
  const auto cash = Money::from_micros(thousands * 1'000'000'000);
  AccountRules rules;
  rules.plan = (fixed ? "Static " : "Locking ") + size;
  rules.buying_power = true;
  rules.slippage_ticks = 0;
  rules.margin = trading::MarginMode::Strategy;
  rules.expiry_cutoff = 5 * md::kNanosPerMinute;
  if (fixed) {
    rules.profit_target = cash.prorate(10, 100);
    rules.max_drawdown = cash.prorate(8, 100);
    rules.drawdown_mode = DrawdownMode::Static;
    rules.profit_basis = trading::ProfitBasis::Balance;
    rules.daily_loss_limit = cash.prorate(4, 100);
    rules.daily_loss_basis = trading::DailyLossBasis::Balance;
    rules.daily_loss_action = trading::BreachAction::Fail;
    rules.min_trading_days = 4;
    rules.day_end_minutes = 18 * 60;
  } else {
    rules.profit_target = cash.prorate(6, 100);
    rules.max_drawdown = cash.prorate(4, 100);
    rules.drawdown_mode = DrawdownMode::EndOfDay;
    rules.lock_at_start = true;
    rules.daily_loss_limit = cash.prorate(2, 100);
    rules.consistency_percent = 50;
  }
  return {std::string(style) + "-" + std::to_string(thousands) + "k", rules.plan,
          fixed ? "Any strategy. 10% target on the closed balance, every position closed; a static floor 8% below the start; "
                  "a 4% loss from a day's opening balance fails the attempt; at least 4 trading days. Days end at 6 pm ET."
                : "Any strategy. 6% target; a 4% drawdown trailing each close that locks at the starting balance; a 2% daily "
                  "loss closes every position and locks the day; no day may make more than half the profit.",
          cash, rules, {}};
}
PlanPreset funded(const PlanPreset& evaluation, std::int64_t qualifying_dollars) {
  const auto& cash = evaluation.initial_cash;
  auto rules = evaluation.rules;
  rules.plan = "Funded " + evaluation.name;
  rules.phase = trading::Phase::Funded;
  rules.profit_target = {};
  rules.lock_balance = cash;
  auto& p = rules.payouts;
  p.qualifying_profit = Money::from_micros(qualifying_dollars * 1'000'000);
  p.qualifying_days = 8;
  p.withdrawal_percent = 50;
  p.split_percent = 80;
  p.minimum = cash.prorate(1, 100);
  for (const std::int64_t percent : {2, 3, 4, 6}) p.caps.push_back(cash.prorate(percent, 100));
  return {"funded-" + evaluation.id, rules.plan,
          "Unlocked by passing " + evaluation.name + ". No target; the trailing floor locks at the starting balance. "
          "Payouts after 8 days of $" + std::to_string(qualifying_dollars) + "+ net realised profit, up to half the profit; you keep 80%.",
          cash, rules, evaluation.id};
}
std::vector<PlanPreset> build() {
  AccountRules practice;
  practice.plan = "Practice";
  practice.buying_power = true;
  practice.slippage_ticks = 0;
  practice.margin = trading::MarginMode::Strategy;
  std::vector<PlanPreset> plans{{"practice", "Practice", "No target or drawdown. Buying power applies.",
                                 Money::from_micros(100'000'000'000), practice, {}}};
  for (const auto* style : {"intraday", "eod"})
    for (const auto size : {25, 50, 100}) plans.push_back(evaluation(style, size));
  // A qualifying day's net realised profit: $100, $150 and $200 for 25K, 50K and 100K.
  for (std::size_t i = 1, n = plans.size(); i < n; ++i)
    plans.push_back(funded(plans[i], std::array<std::int64_t, 3>{100, 150, 200}[(i - 1) % 3]));
  for (const auto* style : {"static", "locking"})
    for (const auto size : {25, 50, 100}) plans.push_back(objective_evaluation(style, size));
  return plans;
}
}  // namespace

const std::vector<PlanPreset>& plan_presets() {
  static const auto plans = build();
  return plans;
}
const PlanPreset* find_plan(std::string_view id) {
  const auto& plans = plan_presets();
  const auto it = std::find_if(plans.begin(), plans.end(), [&](const auto& plan) { return plan.id == id; });
  return it == plans.end() ? nullptr : &*it;
}
const PlanPreset* find_plan_named(std::string_view name) {
  const auto& plans = plan_presets();
  const auto it = std::find_if(plans.begin(), plans.end(), [&](const auto& plan) { return plan.name == name; });
  return it == plans.end() ? nullptr : &*it;
}
bool follows_plan(const PlanPreset& plan, Money initial_cash, const AccountRules& rules) {
  auto execution = rules;
  execution.slippage_ticks = plan.rules.slippage_ticks;
  execution.fill_latency_ms = plan.rules.fill_latency_ms;
  execution.impact_ticks = plan.rules.impact_ticks;
  return initial_cash == plan.initial_cash && execution == plan.rules;
}

}  // namespace openport::server

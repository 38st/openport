#include "openport/server/plans.hpp"

#include <algorithm>

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
  rules.expiry_cutoff = 5 * md::kNanosPerMinute;
  return {std::string(style) + "-" + std::to_string(thousands) + "k", rules.plan,
          intraday ? "Buy-only single-leg options. 10% profit target; 5% trailing drawdown that rises with every new equity high."
                   : "Any strategy. 12% profit target; 6% trailing drawdown that rises only with each day's closing equity.",
          cash, rules};
}
std::vector<PlanPreset> build() {
  AccountRules practice;
  practice.plan = "Practice";
  practice.buying_power = true;
  std::vector<PlanPreset> plans{{"practice", "Practice", "No target or drawdown. Buying power applies.",
                                 Money::from_micros(100'000'000'000), practice}};
  for (const auto* style : {"intraday", "eod"})
    for (const auto size : {25, 50, 100}) plans.push_back(evaluation(style, size));
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

}  // namespace openport::server

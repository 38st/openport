#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "openport/trading/types.hpp"

namespace openport::server {

/// A named starting balance and rule set offered for new journals and resets.
struct PlanPreset {
  std::string id;       ///< Stable identifier, e.g. "intraday-100k".
  std::string name;     ///< Display name, recorded as AccountRules::plan.
  std::string summary;  ///< One line describing the rules.
  trading::Money initial_cash;
  trading::AccountRules rules;
};

/// Practice (buying power only), then Intraday and End-of-day evaluations at
/// 25K, 50K and 100K:
///   intraday: buy-only single-leg, 10% target, 5% trailing drawdown ratcheting on every high
///   eod:      any strategy, 12% target, 6% trailing drawdown ratcheting at each close
/// Evaluations auto-close positions five minutes before expiry.
[[nodiscard]] const std::vector<PlanPreset>& plan_presets();
[[nodiscard]] const PlanPreset* find_plan(std::string_view id);

}  // namespace openport::server

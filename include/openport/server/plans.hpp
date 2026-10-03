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
  std::string unlocked_by;  ///< Funded plans: the evaluation preset whose pass unlocks a reset into it.
};

/// Practice (buying power only), then Intraday and End-of-day evaluations at
/// 25K, 50K and 100K, then the funded account each of those unlocks, then Static
/// and Locking evaluations at the same sizes:
///   intraday: buy-only single-leg, 10% target, 5% trailing drawdown ratcheting on every high
///   eod:      any strategy, 12% target, 6% trailing drawdown ratcheting at each close
///   funded:   the evaluation's rules without a target; the floor locks at the
///             starting balance; a payout every 8 days of $100/$150/$200+ net
///             realised profit (25K/50K/100K), up to 50% of profit (minimum 1%,
///             caps 2/3/4/6% of the balance for payouts 1/2/3/4+), 80% to the trader
///   static:   any strategy, 10% target on the closed balance, a static floor 8% below
///             the start, a 4% daily loss from the day's opening balance fails, at
///             least 4 trading days, days ending at 18:00 New York time
///   locking:  any strategy, 6% target, 4% drawdown trailing each close that locks
///             at the starting balance, a 2% daily loss locks the day, best day at
///             most 50% of the profit
/// Evaluations auto-close positions five minutes before their last trade.
[[nodiscard]] const std::vector<PlanPreset>& plan_presets();
[[nodiscard]] const PlanPreset* find_plan(std::string_view id);
/// The preset with this display name (`AccountRules::plan`), if any.
[[nodiscard]] const PlanPreset* find_plan_named(std::string_view name);
/// Whether an attempt that started with this balance and these rules is the preset's
/// own. Only the fill model's execution settings (slippage, latency, impact, inside fills) and the
/// account's margin (mode, account type, house margin, vol shock) may differ.
[[nodiscard]] bool follows_plan(const PlanPreset& plan, trading::Money initial_cash, const trading::AccountRules& rules);

}  // namespace openport::server

#pragma once

// Internal test seam, compiled only when sanitizer tests are enabled. The default
// leaves generation unchanged, including in the separately launched CLI tests.
#include <atomic>

#include "openport/providers/scenario.hpp"

namespace openport::providers::detail {
inline std::atomic_bool sample_scenario_snapshots{false};

inline bool keep_scenario_snapshot(md::Timestamp now, const ScenarioWindow& window) {
  if (!sample_scenario_snapshots.load(std::memory_order_relaxed)) return true;
  // Keep opening fills/management and exact close/last-trade boundaries, including
  // the quote just before retirement and the paced replay's final 15-second gap.
  return now <= window.first + 10 * md::kNanosPerMinute ||
      now % (5 * md::kNanosPerMinute) == 0 ||
      now == window.close - window.step || now == window.close ||
      now >= window.last - window.step;
}
}  // namespace openport::providers::detail

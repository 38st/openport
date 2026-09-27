#pragma once

#include <span>
#include "openport/analytics/realized.hpp"

namespace openport::analytics {
struct ComparisonSample { md::Timestamp as_of; double value; };
struct ComparisonStats {
  std::size_t samples = 0;
  double median_absolute = kNaN;
  double p90_absolute = kNaN;
  double max_absolute = kNaN;
  double median_signed = kNaN;
};
[[nodiscard]] ComparisonStats comparison_stats(std::span<const double> signed_differences);
/// Cboe bars have been converted to start times by parse_cboe_intraday. Match
/// their closing-minute label to the sample's market minute; never nearest-time.
/// Keep the last sample per minute, so polling frequency cannot overweight it.
[[nodiscard]] ComparisonStats compare_minutes(std::span<const ComparisonSample> samples,
                                               std::span<const md::Bar> reference);
}  // namespace openport::analytics

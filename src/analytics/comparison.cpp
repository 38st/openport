#include "openport/analytics/comparison.hpp"

#include <cmath>
#include <map>

namespace openport::analytics {
ComparisonStats comparison_stats(std::span<const double> differences) {
  std::vector<double> absolute, signed_values;
  for (double value : differences) {
    if (!std::isfinite(value)) continue;
    absolute.push_back(std::abs(value)); signed_values.push_back(value);
  }
  return {absolute.size(), percentile(absolute, 0.5), percentile(absolute, 0.9),
          percentile(absolute, 1), percentile(signed_values, 0.5)};
}

ComparisonStats compare_minutes(std::span<const ComparisonSample> samples, std::span<const md::Bar> reference) {
  std::map<md::Timestamp, double> bars, own;
  auto minute = [](md::Timestamp time) {
    const auto remainder = time % md::kNanosPerMinute;
    return time - remainder - (remainder < 0 ? md::kNanosPerMinute : 0);
  };
  for (const auto& bar : reference)
    if (md::valid_bar(bar)) bars[minute(bar.start + md::kNanosPerMinute)] = bar.close;
  for (const auto& sample : samples)
    own[minute(sample.as_of)] = sample.value;
  std::vector<double> differences;
  for (const auto& [time, value] : own) {
    const auto found = bars.find(time);
    if (found != bars.end() && std::isfinite(value)) differences.push_back(value - found->second);
  }
  return comparison_stats(differences);
}
}  // namespace openport::analytics

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <span>

namespace openport::analytics {
// Closes are chronological. Callers choose completed bars and a stable history
// window; missing or invalid observations never become synthetic prices.
inline bool technical_history(std::span<const double> closes, std::size_t needed) {
  return needed > 0 && closes.size() >= needed &&
      std::all_of(closes.begin(), closes.end(), [](double value) { return std::isfinite(value) && value > 0; });
}
inline double sma(std::span<const double> closes, std::size_t period) {
  if (!technical_history(closes, period)) return std::numeric_limits<double>::quiet_NaN();
  const auto tail = closes.last(period);
  return std::accumulate(tail.begin(), tail.end(), 0.0) / static_cast<double>(period);
}
inline double ema(std::span<const double> closes, std::size_t period) {
  if (!technical_history(closes, period)) return std::numeric_limits<double>::quiet_NaN();
  double value = sma(closes.first(period), period);
  const double alpha = 2.0 / static_cast<double>(period + 1);
  for (std::size_t i = period; i < closes.size(); ++i) value += alpha * (closes[i] - value);
  return value;
}
inline double rsi(std::span<const double> closes, std::size_t period) {
  if (period == 0 || !technical_history(closes, period + 1)) return std::numeric_limits<double>::quiet_NaN();
  double gain = 0, loss = 0;
  for (std::size_t i = 1; i < closes.size(); ++i) {
    const double change = closes[i] - closes[i - 1];
    if (i <= period) { gain += std::max(0.0, change); loss += std::max(0.0, -change); }
    else {
      gain = (gain * static_cast<double>(period - 1) + std::max(0.0, change)) / static_cast<double>(period);
      loss = (loss * static_cast<double>(period - 1) + std::max(0.0, -change)) / static_cast<double>(period);
    }
    if (i == period) { gain /= static_cast<double>(period); loss /= static_cast<double>(period); }
  }
  return gain + loss == 0 ? 50 : 100 * gain / (gain + loss);
}
inline double bollinger(std::span<const double> closes, std::size_t period, double deviations) {
  const double mean = sma(closes, period);
  if (!std::isfinite(mean) || !std::isfinite(deviations)) return std::numeric_limits<double>::quiet_NaN();
  double variance = 0;
  for (double value : closes.last(period)) variance += (value - mean) * (value - mean);
  return mean + deviations * std::sqrt(variance / static_cast<double>(period));
}
}  // namespace openport::analytics

#pragma once

#include <algorithm>
#include <cmath>

#include "openport/md/time.hpp"

namespace openport::md {

/// One OHLC price bar of an underlying. `start` is the instant the bar opens, in
/// market-data time: a minute bar covers [start, start + 1 min), and a daily bar
/// starts at its session's 09:30 ET open.
struct Bar {
  Timestamp start = 0;
  double open = 0.0;
  double high = 0.0;
  double low = 0.0;
  double close = 0.0;

  friend bool operator==(const Bar&, const Bar&) = default;
};

/// Finite, positive prices whose high and low bracket the open and close.
[[nodiscard]] inline bool valid_bar(const Bar& bar) noexcept {
  return std::isfinite(bar.open) && std::isfinite(bar.high) && std::isfinite(bar.low) &&
         std::isfinite(bar.close) && bar.low > 0.0 && bar.low <= std::min(bar.open, bar.close) &&
         bar.high >= std::max(bar.open, bar.close);
}

}  // namespace openport::md

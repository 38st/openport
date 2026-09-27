#pragma once

#include <array>
#include <span>

#include "openport/analytics/chain_analytics.hpp"
#include "openport/md/bars.hpp"

namespace openport::analytics {
inline constexpr std::array<int, 6> kRealizedWindows{5, 10, 21, 63, 126, 252};
inline constexpr double kSessionsPerYear = 252.0;

struct RealizedEstimate {
  double vol = kNaN;
  bool fallback = false;
  std::string reason;
};
struct RealizedWindow {
  int sessions = 0;
  RealizedEstimate close_to_close;
  RealizedEstimate parkinson;
  RealizedEstimate garman_klass;
  RealizedEstimate yang_zhang;
};
/// N returns need N+1 bars; OHLC estimators use the last N bars. Decimal log
/// returns, unbiased demeaned close/overnight/open-close variances, 252 sessions.
[[nodiscard]] RealizedWindow realized_window(std::span<const md::Bar> bars, int sessions);
[[nodiscard]] double percentile(std::span<const double> values, double probability);

struct VolCone {
  int sessions = 0;
  std::size_t days_used = 0;
  std::size_t observations = 0;
  double min = kNaN, p10 = kNaN, p25 = kNaN, p50 = kNaN, p75 = kNaN, p90 = kNaN, max = kNaN;
  double current = kNaN;
  double current_percentile = kNaN;
};
[[nodiscard]] VolCone vol_cone(std::span<const md::Bar> bars, int sessions);

struct IntradayRealized {
  md::Date date;
  double vol = kNaN;
  int returns = 0;
  int observed_minutes = 0;
  int session_minutes = 0;
  bool partial = true;
  std::string reason;
};
/// Input is one-minute bars. Complete five-minute blocks only, no overnight or
/// gap-spanning returns. The first block uses its open, later blocks the prior close.
[[nodiscard]] std::vector<IntradayRealized> intraday_realized(std::span<const md::Bar> minutes,
                                                            md::Timestamp as_of);
struct RealizedMetrics {
  std::string daily_as_of;  ///< Date of the last completed daily bar actually available.
  std::vector<RealizedWindow> windows;
  std::vector<VolCone> cones;
  std::vector<IntradayRealized> intraday;
  double today = kNaN;
};
/// Excludes future/unfinished sessions and respects the md regular-session calendar.
[[nodiscard]] RealizedMetrics realized_metrics(std::span<const md::Bar> days,
                                               std::span<const md::Bar> minutes,
                                               md::Timestamp as_of);
}  // namespace openport::analytics

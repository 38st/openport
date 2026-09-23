#pragma once

#include <span>
#include <string>
#include <vector>

#include "openport/analytics/chain_analytics.hpp"

namespace openport::analytics {

/// Raw SVI in TOTAL variance, not annual variance. IV(k) = sqrt(w(k) / T).
struct SviParameters {
  double a = kNaN;
  double b = kNaN;
  double rho = kNaN;
  double m = kNaN;
  double sigma = kNaN;
};

struct SviPoint {
  double k;
  double iv;
  double bid_iv;
  double ask_iv;
};

enum class SviStatus { Ok, TooFewPoints, Failed };
[[nodiscard]] const char* to_string(SviStatus status);

struct SviButterfly {
  double min_g = kNaN;
  double k = kNaN;
  bool ok = false;
};

struct SviFit {
  SviParameters parameters;
  SviStatus status = SviStatus::Failed;
  std::string reason;
  double rmse_vol_points = kNaN;
  std::size_t points = 0;
  double years = kNaN;
  double min_k = kNaN;
  double max_k = kNaN;
  double fit_ms = 0.0;
  SviButterfly butterfly;
};

[[nodiscard]] double svi_variance(const SviParameters& p, double k);
[[nodiscard]] double svi_iv(const SviParameters& p, double k, double years);
/// Undefined density (including zero variance) is NaN, never a passing check.
[[nodiscard]] double svi_density(const SviParameters& p, double k);
/// Includes both endpoints on a 2,001-point grid; this is a diagnostic, not a proof.
[[nodiscard]] SviButterfly svi_butterfly(const SviParameters& p, double min_k, double max_k);
/// Checks the total-variance Lee bound AND the requested additional 4/T cap.
[[nodiscard]] bool svi_admissible(const SviParameters& p, double years);

/// Deterministic quasi-explicit weighted least squares in total variance. At least
/// five distinct two-sided IV points are required. Weights are inverse squared IV
/// spreads, floored at 1bp vol and capped at four times their median and 25% of sum.
[[nodiscard]] SviFit fit_svi(std::span<const SviPoint> points, double years);
/// Uses the chain's OTM smile and the matching side's two-sided price/IV market.
[[nodiscard]] SviFit fit_svi(const SliceMetrics& slice);

struct SviCalendarViolation {
  std::size_t earlier;
  std::size_t later;
  double k;
  double vol_points;  ///< Later expiry's IV increase needed to remove the violation.
};
/// Checks consecutive successful tenors over the intersection of fitted ranges.
/// Reports the largest later-expiry IV increase exceeding max(0.1 vp, both RMSEs).
/// Earlier/later are indices into fits; the 2,001-point grid includes endpoints.
[[nodiscard]] std::vector<SviCalendarViolation> svi_calendar(std::span<const SviFit> fits);

}  // namespace openport::analytics

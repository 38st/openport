#pragma once

#include "openport/analytics/svi.hpp"

namespace openport::analytics {

struct SsviParameters {
  double rho = kNaN;
  double eta = kNaN;
  double gamma = kNaN;
};

struct SsviSlice {
  double years;
  std::vector<SviPoint> points;
};

struct SsviExpiryFit {
  double theta = kNaN;
  double rmse_vol_points = kNaN;
  double min_k = kNaN;
  double max_k = kNaN;
  std::size_t points = 0;
  std::string reason;  ///< Missing ATM bracket/invalid tenor/insufficient quotes.
};

struct SsviFit {
  SsviParameters parameters;
  SviStatus status = SviStatus::Failed;
  std::string reason;
  bool monotone_adjusted = false;
  double rmse_vol_points = kNaN;
  double fit_ms = 0;
  std::vector<SsviExpiryFit> expiries;  ///< Same order as input; skipped theta is NaN.
};

/// Modified power law, G&J (2014), Eq. (4.5), Theorems 4.1/4.2, Corollary 4.1.
/// With positive nondecreasing theta: |rho|<1, eta>0, 0<gamma<=1/2,
/// eta(1+|rho|)<=2 suffice for no static arbitrage for all k and theta>0.
[[nodiscard]] bool ssvi_admissible(const SsviParameters& p);
[[nodiscard]] double ssvi_variance(const SsviParameters& p, double k, double theta);
[[nodiscard]] double ssvi_iv(const SsviParameters& p, double k, double theta, double years);
/// Exact raw-SVI representation for the existing density/calendar diagnostics.
[[nodiscard]] SviParameters ssvi_slice(const SsviParameters& p, double theta);

/// ATM IV is linearly interpolated at k=0 from eligible two-sided OTM quotes.
/// Equal-weight isotonic regression repairs theta, pooling equal tenors too.
/// Then deterministic bounded least squares fits rho/eta/gamma in total variance,
/// with the same per-expiry normalized quote weights as SVI (equal expiry weight).
/// At least two usable distinct tenors with five distinct quotes each are needed.
/// Unusable expiries retain their reason and do not enter the surface calibration.
[[nodiscard]] SsviFit fit_ssvi(std::span<const SsviSlice> slices);
[[nodiscard]] SsviFit fit_ssvi(std::span<const SliceMetrics> slices);

}  // namespace openport::analytics

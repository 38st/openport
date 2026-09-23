#pragma once

#include "openport/analytics/svi.hpp"

namespace openport::analytics::detail {
struct SviDatum { double k, iv, w, weight, half_spread; };
// Shared eligibility and per-expiry normalized weights for SVI and SSVI.
std::vector<SviDatum> prepare_svi(std::span<const SviPoint> points, double years);
std::vector<SviPoint> svi_points(const SliceMetrics& slice);
}  // namespace openport::analytics::detail

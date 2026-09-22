#include "openport/analytics/forward.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace openport::analytics {
namespace {

struct Line {
  double intercept = 0.0;
  double slope = 0.0;
  bool ok = false;
};

/// Weighted least squares for y = a + b x over the points flagged in `use`.
Line fit(std::span<const ParityPoint> points, const std::vector<bool>& use) {
  double sw = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  int n = 0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (!use[i]) continue;
    const double w = points[i].weight;
    const double x = points[i].strike;
    const double y = points[i].call_mid - points[i].put_mid;
    sw += w;
    sx += w * x;
    sy += w * y;
    sxx += w * x * x;
    sxy += w * x * y;
    ++n;
  }
  Line line;
  const double denominator = sw * sxx - sx * sx;
  if (n < 2 || !(sw > 0.0) || std::abs(denominator) <= 1e-12 * sw * sxx) return line;
  line.slope = (sw * sxy - sx * sy) / denominator;
  line.intercept = (sy - line.slope * sx) / sw;
  line.ok = std::isfinite(line.slope) && std::isfinite(line.intercept);
  return line;
}

}  // namespace

ForwardEstimate implied_forward(std::span<const ParityPoint> points, double expiry_years,
                                double fallback_rate) {
  ForwardEstimate out;
  if (points.empty() || !(expiry_years > 0.0)) return out;

  std::vector<bool> use(points.size(), true);
  const double min_discount = std::exp(-0.20 * expiry_years);
  const double max_discount = std::exp(0.05 * expiry_years);
  auto plausible = [&](const Line& line) {
    const double discount = -line.slope;
    return line.ok && discount >= min_discount && discount <= max_discount;
  };

  if (points.size() >= 3) {
    Line line = fit(points, use);
    if (line.ok) {
      // One pass of outlier removal: stale or crossed quotes can sit far off the line.
      double sw = 0.0, swr2 = 0.0;
      for (std::size_t i = 0; i < points.size(); ++i) {
        const double r = points[i].call_mid - points[i].put_mid -
                         (line.intercept + line.slope * points[i].strike);
        sw += points[i].weight;
        swr2 += points[i].weight * r * r;
      }
      const double sigma = std::sqrt(swr2 / sw);
      int kept = 0;
      for (std::size_t i = 0; i < points.size(); ++i) {
        const double r = points[i].call_mid - points[i].put_mid -
                         (line.intercept + line.slope * points[i].strike);
        use[i] = std::abs(r) <= 3.0 * sigma + 1e-12;
        kept += use[i] ? 1 : 0;
      }
      if (kept >= 3 && kept < static_cast<int>(points.size())) line = fit(points, use);
      if (plausible(line)) {
        out.discount = -line.slope;
        out.forward = line.intercept / out.discount;
        out.points = kept;
        out.fitted_discount = true;
        out.ok = out.forward > 0.0;
        if (out.ok) return out;
      }
    }
    std::fill(use.begin(), use.end(), true);
  }

  // Too few strikes, or a slope the data cannot pin down: assume the discount factor.
  return implied_forward_given_discount(points, std::exp(-fallback_rate * expiry_years));
}

ForwardEstimate implied_forward_given_discount(std::span<const ParityPoint> points,
                                               double discount) {
  ForwardEstimate out;
  out.discount = discount;
  if (points.empty() || !(discount > 0.0)) return out;

  auto implied = [discount](const ParityPoint& p) {
    return p.strike + (p.call_mid - p.put_mid) / discount;
  };
  auto mean = [&](const std::vector<bool>& use, int& used) {
    double sw = 0.0, swf = 0.0;
    used = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
      if (!use[i]) continue;
      sw += points[i].weight;
      swf += points[i].weight * implied(points[i]);
      ++used;
    }
    return sw > 0.0 ? swf / sw : 0.0;
  };

  std::vector<bool> use(points.size(), true);
  int used = 0;
  double forward = mean(use, used);
  if (points.size() >= 3) {
    double sw = 0.0, swr2 = 0.0;
    for (const ParityPoint& p : points) {
      const double r = implied(p) - forward;
      sw += p.weight;
      swr2 += p.weight * r * r;
    }
    const double sigma = std::sqrt(swr2 / sw);
    for (std::size_t i = 0; i < points.size(); ++i) {
      use[i] = std::abs(implied(points[i]) - forward) <= 3.0 * sigma + 1e-12;
    }
    forward = mean(use, used);
  }
  out.forward = forward;
  out.points = used;
  out.ok = forward > 0.0 && used > 0;
  return out;
}

}  // namespace openport::analytics

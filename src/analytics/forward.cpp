#include "openport/analytics/forward.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace openport::analytics {
namespace {

double median(std::vector<double> values) {
  const auto middle = values.begin() + (values.size() - 1) / 2;
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

// The absolute cap corresponds to a one-cent combined spread. The relative cap
// prevents a lone locked quote from outweighing even a sparse wider market.
std::vector<ParityPoint> bounded_points(std::span<const ParityPoint> points) {
  std::vector<ParityPoint> out;
  std::vector<double> weights;
  for (const auto& p : points) {
    if (!std::isfinite(p.strike) || p.strike <= 0 || !std::isfinite(p.call_mid) || p.call_mid < 0 ||
        !std::isfinite(p.put_mid) || p.put_mid < 0 || !std::isfinite(p.weight) || p.weight <= 0)
      continue;
    out.push_back(p);
    weights.push_back(p.weight);
  }
  if (out.empty()) return out;
  const double cap = std::min(1e4, median(weights));
  for (auto& p : out) p.weight = std::min(p.weight, cap);
  return out;
}

std::vector<bool> inliers(const std::vector<ParityPoint>& points, double discount) {
  std::vector<std::pair<double, double>> forwards;
  double total_weight = 0;
  for (const auto& p : points) {
    forwards.emplace_back(p.strike + (p.call_mid - p.put_mid) / discount, p.weight);
    total_weight += p.weight;
  }
  std::sort(forwards.begin(), forwards.end());
  double center = forwards.front().first;
  double cumulative = 0;
  for (const auto& [f, w] : forwards) {
    cumulative += w;
    center = f;
    if (cumulative >= total_weight / 2) break;
  }
  std::vector<double> residuals;
  for (const auto& [f, w] : forwards) residuals.push_back(std::abs(f - center));
  // MAD does not grow with a single stale price. The numerical floor preserves
  // exact parity points when roundoff makes their theoretical zero MAD nonzero.
  const double cutoff =
      std::max(3.0 * 1.4826 * median(residuals), 1e-10 * std::max(1.0, std::abs(center)));
  std::vector<bool> use;
  for (const auto& p : points) {
    const double f = p.strike + (p.call_mid - p.put_mid) / discount;
    use.push_back(std::isfinite(f) && (points.size() < 3 || std::abs(f - center) <= cutoff));
  }
  return use;
}

struct Line {
  double intercept = 0.0;
  double slope = 0.0;
  int points = 0;
  bool ok = false;
};

Line fit(const std::vector<ParityPoint>& points, const std::vector<bool>& use) {
  double sw = 0, sx = 0, sy = 0;
  Line line;
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (!use[i]) continue;
    const auto& p = points[i];
    sw += p.weight;
    sx += p.weight * p.strike;
    sy += p.weight * (p.call_mid - p.put_mid);
    ++line.points;
  }
  if (line.points < 3 || !(sw > 0)) return line;
  const double xbar = sx / sw, ybar = sy / sw;
  double sxx = 0, sxy = 0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (!use[i]) continue;
    const auto& p = points[i];
    const double x = p.strike - xbar;
    sxx += p.weight * x * x;
    sxy += p.weight * x * (p.call_mid - p.put_mid - ybar);
  }
  if (!(sxx > 1e-12 * sw)) return line;
  line.slope = sxy / sxx;
  line.intercept = ybar - line.slope * xbar;
  line.ok = std::isfinite(line.slope) && std::isfinite(line.intercept);
  return line;
}

}  // namespace

ForwardEstimate implied_forward(std::span<const ParityPoint> input, double expiry_years,
                                double fallback_rate) {
  if (!std::isfinite(expiry_years) || !(expiry_years > 0) || !std::isfinite(fallback_rate))
    return {};
  const auto points = bounded_points(input);
  if (points.empty()) return {};
  const double discount = std::exp(-fallback_rate * expiry_years);
  if (!(discount > 0) || !std::isfinite(discount)) return {};
  const auto line = fit(points, inliers(points, discount));
  if (line.ok && -line.slope >= std::exp(-0.20 * expiry_years) &&
      -line.slope <= std::exp(0.05 * expiry_years)) {
    const double forward = line.intercept / -line.slope;
    if (std::isfinite(forward) && forward > 0)
      return {forward, -line.slope, line.points, true, true};
  }
  return implied_forward_given_discount(points, discount);
}

ForwardEstimate implied_forward_given_discount(std::span<const ParityPoint> input,
                                               double discount) {
  ForwardEstimate out;
  out.discount = discount;
  if (!(discount > 0) || !std::isfinite(discount)) return out;
  const auto points = bounded_points(input);
  if (points.empty()) return out;
  const auto use = inliers(points, discount);
  double sw = 0, swf = 0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (!use[i]) continue;
    const auto& p = points[i];
    sw += p.weight;
    swf += p.weight * (p.strike + (p.call_mid - p.put_mid) / discount);
    ++out.points;
  }
  out.forward = sw > 0 ? swf / sw : 0;
  out.ok = std::isfinite(out.forward) && out.forward > 0 && out.points > 0;
  return out;
}

}  // namespace openport::analytics

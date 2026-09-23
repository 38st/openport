#include "openport/pricing/binomial.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace openport::pricing {
namespace {

/// Peizer-Pratt method 2 inversion: the probability a binomial with n steps
/// assigns to the normal quantile z.
[[nodiscard]] double peizer_pratt(double z, int n) noexcept {
  const double nd = static_cast<double>(n);
  const double a = z / (nd + 1.0 / 3.0 + 0.1 / (nd + 1.0));
  const double root = std::sqrt(0.25 - 0.25 * std::exp(-a * a * (nd + 1.0 / 6.0)));
  return z >= 0.0 ? 0.5 + root : 0.5 - root;
}

// With no randomness, exercise can be optimal before maturity. Check both
// endpoints and the sole stationary point of the discounted exercise payoff.
double deterministic_american(const BsmInputs& in) {
  auto payoff = [&](double t) {
    return std::max(omega(in.type) *
                        (in.spot * std::exp(-in.dividend * t) - in.strike * std::exp(-in.rate * t)),
                    0.0);
  };
  double best = std::max(payoff(0), payoff(std::max(in.expiry, 0.0)));
  if (in.rate * in.dividend > 0 && in.rate != in.dividend) {
    const double t =
        std::log(in.dividend * in.spot / (in.rate * in.strike)) / (in.dividend - in.rate);
    if (t > 0 && t < in.expiry) best = std::max(best, payoff(t));
  }
  return best;
}

}  // namespace

double binomial_price(const BsmInputs& in, ExerciseStyle style, TreeMethod method, int steps) {
  if (!std::isfinite(in.spot) || !std::isfinite(in.strike) || !std::isfinite(in.expiry) ||
      !std::isfinite(in.rate) || !std::isfinite(in.dividend) || !std::isfinite(in.vol) ||
      in.spot <= 0 || in.strike <= 0 || in.vol < 0) {
    throw std::invalid_argument(
        "binomial inputs must be finite with positive spot/strike and nonnegative vol");
  }
  const double w = omega(in.type);
  auto fallback = [&] {
    double value = std::max(bsm_price(in), 0.0);
    if (style == ExerciseStyle::American) value = std::max(value, deterministic_american(in));
    if (!std::isfinite(value)) throw std::overflow_error("binomial fallback price is not finite");
    return value;
  };
  if (!(in.expiry > 0.0) || !(in.vol > 0.0) || steps < 1) return fallback();
  if (method == TreeMethod::LeisenReimer && steps % 2 == 0) ++steps;

  const double dt = in.expiry / steps;
  const double growth = std::exp((in.rate - in.dividend) * dt);
  const double discount = std::exp(-in.rate * dt);

  double up = 0.0;
  double down = 0.0;
  double p = 0.0;
  if (method == TreeMethod::CoxRossRubinstein) {
    up = std::exp(in.vol * std::sqrt(dt));
    down = 1.0 / up;
    p = (growth - down) / (up - down);
  } else {
    const double sd = in.vol * std::sqrt(in.expiry);
    const double d1 =
        (std::log(in.spot / in.strike) + (in.rate - in.dividend) * in.expiry) / sd + 0.5 * sd;
    const double d2 = d1 - sd;
    p = peizer_pratt(d2, steps);
    const double p_star = peizer_pratt(d1, steps);
    // Saturated normal tails otherwise divide by zero (or subtract equal numbers).
    if (!(p > 0.0 && p < 1.0 && p_star > 0.0 && p_star < 1.0)) return fallback();
    up = growth * p_star / p;
    down = growth * (1.0 - p_star) / (1.0 - p);
  }
  // CRR is inadmissible when carry exceeds a one-step move. Signed branch
  // weights are not probabilities and can produce negative option prices.
  if (!std::isfinite(p) || p < 0.0 || p > 1.0 || !std::isfinite(up) || !std::isfinite(down) ||
      !(up > 0.0) || !(down > 0.0) || !std::isfinite(discount))
    return fallback();
  const double discounted_up = discount * p;
  const double discounted_down = discount * (1.0 - p);

  // spot[j] is the price after j up-moves; walking back one level divides by `down`.
  std::vector<double> spot(static_cast<std::size_t>(steps) + 1);
  std::vector<double> value(static_cast<std::size_t>(steps) + 1);
  spot[0] = in.spot * std::pow(down, steps);
  if (!(spot[0] > 0.0) || !std::isfinite(spot[0]) || !std::isfinite(in.spot * std::pow(up, steps)))
    return fallback();
  const double up_over_down = up / down;
  for (int j = 0; j <= steps; ++j) {
    if (j > 0) spot[j] = spot[j - 1] * up_over_down;
    value[j] = std::max(w * (spot[j] - in.strike), 0.0);
  }

  const bool american = style == ExerciseStyle::American;
  for (int level = steps - 1; level >= 0; --level) {
    for (int j = 0; j <= level; ++j) {
      double v = discounted_down * value[j] + discounted_up * value[j + 1];
      spot[j] /= down;
      if (american) v = std::max(v, w * (spot[j] - in.strike));
      value[j] = v;
    }
  }
  return std::isfinite(value[0]) && value[0] >= 0.0 ? value[0] : fallback();
}

}  // namespace openport::pricing

#include "openport/pricing/binomial.hpp"

#include <algorithm>
#include <cmath>
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

}  // namespace

double binomial_price(const BsmInputs& in, ExerciseStyle style, TreeMethod method, int steps) {
  const double w = omega(in.type);
  if (!(in.expiry > 0.0) || !(in.vol > 0.0) || steps < 1) {
    return bsm_price(in);  // no tree to build: fall back to the deterministic value
  }
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
    up = growth * p_star / p;
    down = (growth - p * up) / (1.0 - p);
  }
  const double discounted_up = discount * p;
  const double discounted_down = discount * (1.0 - p);

  // spot[j] is the price after j up-moves; walking back one level divides by `down`.
  std::vector<double> spot(static_cast<std::size_t>(steps) + 1);
  std::vector<double> value(static_cast<std::size_t>(steps) + 1);
  spot[0] = in.spot * std::pow(down, steps);
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
  return value[0];
}

}  // namespace openport::pricing

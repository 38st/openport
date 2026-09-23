#include "openport/pricing/black.hpp"

#include <algorithm>
#include <cmath>

#include "openport/pricing/normal.hpp"

namespace openport::pricing {
namespace {

[[nodiscard]] bool no_optionality(double expiry, double vol) noexcept {
  return !(expiry > 0.0) || !(vol > 0.0);
}

/// Intrinsic value and its slope; deterministic carry sensitivities are added
/// by the caller only when there is time left.
[[nodiscard]] Greeks intrinsic_greeks(OptionType type, double forward, double strike,
                                      double discount, double delta_scale) noexcept {
  Greeks g;
  const double w = omega(type);
  const double moneyness = w * (forward - strike);
  if (moneyness > 0.0) {
    g.price = discount * moneyness;
    g.delta = discount * w * delta_scale;
  }
  return g;
}

}  // namespace

double black_price(OptionType type, double forward, double strike, double expiry, double vol,
                   double discount) noexcept {
  const double w = omega(type);
  if (!(expiry > 0.0)) return std::max(w * (forward - strike), 0.0);
  if (!(vol > 0.0)) return discount * std::max(w * (forward - strike), 0.0);

  const double sd = vol * std::sqrt(expiry);
  const double d1 = std::log(forward / strike) / sd + 0.5 * sd;
  const double d2 = d1 - sd;
  return discount * w * (forward * norm_cdf(w * d1) - strike * norm_cdf(w * d2));
}

Greeks black_greeks(OptionType type, double forward, double strike, double expiry, double vol,
                    double discount) noexcept {
  if (!(expiry > 0.0)) return intrinsic_greeks(type, forward, strike, 1.0, 1.0);
  if (!(vol > 0.0)) {
    Greeks g = intrinsic_greeks(type, forward, strike, discount, 1.0);
    g.theta = -std::log(discount) / expiry * g.price;
    g.rho = -expiry * g.price;
    return g;
  }

  const double w = omega(type);
  const double sqrt_t = std::sqrt(expiry);
  const double sd = vol * sqrt_t;
  const double d1 = std::log(forward / strike) / sd + 0.5 * sd;
  const double d2 = d1 - sd;
  const double pdf = norm_pdf(d1);
  const double rate = -std::log(discount) / expiry;

  Greeks g;
  g.price = discount * w * (forward * norm_cdf(w * d1) - strike * norm_cdf(w * d2));
  g.delta = discount * w * norm_cdf(w * d1);
  g.gamma = discount * pdf / (forward * sd);
  g.vega = discount * forward * pdf * sqrt_t;
  g.theta = rate * g.price - discount * forward * pdf * vol / (2.0 * sqrt_t);
  g.rho = -expiry * g.price;
  g.vanna = -discount * pdf * d2 / vol;
  g.volga = g.vega * d1 * d2 / vol;
  return g;
}

double bsm_price(const BsmInputs& in) noexcept {
  const double t = std::max(in.expiry, 0.0);
  const double discount = std::exp(-in.rate * t);
  const double forward = in.spot * std::exp((in.rate - in.dividend) * t);
  return black_price(in.type, forward, in.strike, in.expiry, in.vol, discount);
}

Greeks bsm_greeks(const BsmInputs& in) noexcept {
  const double t = std::max(in.expiry, 0.0);
  const double df_rate = std::exp(-in.rate * t);
  const double df_div = std::exp(-in.dividend * t);
  if (no_optionality(in.expiry, in.vol)) {
    // Value is max(w * (S e^{-qT} - K e^{-rT}), 0); spot delta is +-e^{-qT} in the money.
    Greeks g = intrinsic_greeks(in.type, in.spot * df_div, in.strike * df_rate, 1.0, df_div);
    if (t > 0.0 && g.price > 0.0) {
      g.theta = omega(in.type) * (in.dividend * in.spot * df_div - in.rate * in.strike * df_rate);
      g.rho = omega(in.type) * in.strike * t * df_rate;
    }
    return g;
  }

  const double w = omega(in.type);
  const double sqrt_t = std::sqrt(t);
  const double sd = in.vol * sqrt_t;
  const double d1 = (std::log(in.spot / in.strike) + (in.rate - in.dividend) * t) / sd + 0.5 * sd;
  const double d2 = d1 - sd;
  const double pdf = norm_pdf(d1);
  const double n1 = norm_cdf(w * d1);
  const double n2 = norm_cdf(w * d2);

  Greeks g;
  g.price = w * (in.spot * df_div * n1 - in.strike * df_rate * n2);
  g.delta = w * df_div * n1;
  g.gamma = df_div * pdf / (in.spot * sd);
  g.vega = in.spot * df_div * pdf * sqrt_t;
  g.theta = -in.spot * df_div * pdf * in.vol / (2.0 * sqrt_t) -
            w * in.rate * in.strike * df_rate * n2 + w * in.dividend * in.spot * df_div * n1;
  g.rho = w * in.strike * t * df_rate * n2;
  g.vanna = -df_div * pdf * d2 / in.vol;
  g.volga = g.vega * d1 * d2 / in.vol;
  return g;
}

}  // namespace openport::pricing

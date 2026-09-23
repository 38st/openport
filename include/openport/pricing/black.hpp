#pragma once

#include "openport/pricing/option.hpp"

namespace openport::pricing {

/// Value and sensitivities of a European option.
///
/// Units: vega and volga are per 1.00 of volatility (divide by 100 for "per vol
/// point"), theta is per year of calendar time, rho is per 1.00 of rate. Delta,
/// gamma and vanna are with respect to the forward for Black-76 and with respect
/// to spot for Black-Scholes-Merton.
struct Greeks {
  double price = 0.0;
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double theta = 0.0;
  double rho = 0.0;
  double vanna = 0.0;  ///< d(delta)/d(vol)
  double volga = 0.0;  ///< d(vega)/d(vol), also called vomma
};

/// Black-76 price of a European option on a forward.
///
/// `discount` is the discount factor to expiry, exp(-r * T). With an expired
/// option (T <= 0) the result is intrinsic value, ignoring discount. Positive-time
/// zero volatility gives discounted intrinsic, including deterministic theta/rho.
[[nodiscard]] double black_price(OptionType type, double forward, double strike, double expiry,
                                 double vol, double discount = 1.0) noexcept;

/// Black-76 price and Greeks. Theta holds the forward fixed as time passes, which
/// is the right convention for options on futures and forwards.
[[nodiscard]] Greeks black_greeks(OptionType type, double forward, double strike, double expiry,
                                  double vol, double discount = 1.0) noexcept;

/// Inputs for Black-Scholes-Merton: a spot asset paying a continuous yield.
struct BsmInputs {
  OptionType type = OptionType::Call;
  double spot = 0.0;
  double strike = 0.0;
  double expiry = 0.0;    ///< years
  double rate = 0.0;      ///< continuously compounded risk-free rate
  double dividend = 0.0;  ///< continuous dividend (or carry) yield
  double vol = 0.0;
};

[[nodiscard]] double bsm_price(const BsmInputs& in) noexcept;

/// Black-Scholes-Merton price and spot Greeks.
[[nodiscard]] Greeks bsm_greeks(const BsmInputs& in) noexcept;

}  // namespace openport::pricing

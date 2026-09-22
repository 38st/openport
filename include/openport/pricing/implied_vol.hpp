#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

#include "openport/pricing/option.hpp"

namespace openport::pricing {

enum class IvStatus : std::uint8_t {
  Ok,
  BelowIntrinsic,  ///< no time value left to explain with volatility
  AboveMaximum,    ///< at or above the no-arbitrage upper bound
  NotConverged,
  InvalidInput,
};

[[nodiscard]] std::string_view to_string(IvStatus status) noexcept;

struct IvResult {
  double vol = std::numeric_limits<double>::quiet_NaN();
  int iterations = 0;
  IvStatus status = IvStatus::InvalidInput;

  [[nodiscard]] bool ok() const noexcept { return status == IvStatus::Ok; }
};

struct IvOptions {
  double vol_tolerance = 1e-12;    ///< stop when a step moves vol by less than this
  double price_tolerance = 1e-14;  ///< stop when |model - target| <= this * target
  double max_vol = 10.0;           ///< top of the search bracket (1000%)
  int max_iterations = 100;
};

/// Implied Black-76 volatility of a discounted option price.
///
/// In-the-money inputs are first turned into the out-of-the-money option with the
/// same strike through put-call parity: the time value carries all the volatility
/// information and is better conditioned on its own.
///
/// The solver runs Newton's method on log(price), which stays well behaved for
/// the tiny prices of far out-of-the-money options. It starts from the Corrado
/// and Miller closed-form estimate, falling back to the inflection point of
/// price as a function of volatility, sqrt(2|ln(F/K)| / T), where vega is
/// largest (Manaster and Koehler). Every step is checked against a bisection
/// bracket, so it converges even where vega is tiny.
[[nodiscard]] IvResult implied_vol_black(double price, OptionType type, double forward,
                                         double strike, double expiry, double discount = 1.0,
                                         const IvOptions& options = {}) noexcept;

/// Implied Black-Scholes-Merton volatility of a spot option.
[[nodiscard]] IvResult implied_vol_bsm(double price, OptionType type, double spot, double strike,
                                       double expiry, double rate, double dividend,
                                       const IvOptions& options = {}) noexcept;

}  // namespace openport::pricing

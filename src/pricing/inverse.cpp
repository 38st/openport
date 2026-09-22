#include "openport/pricing/inverse.hpp"

namespace openport::pricing::inverse {

double coin_price(OptionType type, double forward, double strike, double expiry,
                  double vol) noexcept {
  return black_price(type, forward, strike, expiry, vol) / forward;
}

IvResult implied_vol(double coin_premium, OptionType type, double forward, double strike,
                     double expiry, const IvOptions& options) noexcept {
  return implied_vol_black(coin_premium * forward, type, forward, strike, expiry, 1.0, options);
}

double premium_adjusted_delta(OptionType type, double forward, double strike, double expiry,
                              double vol) noexcept {
  const Greeks g = black_greeks(type, forward, strike, expiry, vol);
  return g.delta - g.price / forward;
}

}  // namespace openport::pricing::inverse

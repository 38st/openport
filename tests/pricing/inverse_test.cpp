#include "openport/pricing/inverse.hpp"

#include <gtest/gtest.h>

namespace {

using openport::pricing::OptionType;
namespace inverse = openport::pricing::inverse;

TEST(Inverse, CoinPriceIsDollarBlackPriceOverForward) {
  const double f = 86545.84, k = 90000.0, t = 0.004235, vol = 0.4348;
  const double coin = inverse::coin_price(OptionType::Call, f, k, t, vol);
  EXPECT_NEAR(coin * f, openport::pricing::black_price(OptionType::Call, f, k, t, vol), 1e-9);
}

TEST(Inverse, ImpliedVolRoundTripsFromCoinPremium) {
  for (OptionType type : {OptionType::Call, OptionType::Put}) {
    const double coin = inverse::coin_price(type, 86000.0, 80000.0, 0.1, 0.52);
    const auto iv = inverse::implied_vol(coin, type, 86000.0, 80000.0, 0.1);
    ASSERT_TRUE(iv.ok());
    EXPECT_NEAR(iv.vol, 0.52, 1e-10);
  }
}

TEST(Inverse, PremiumAdjustedDeltaIsForwardTimesCoinPriceSlope) {
  // delta - premium/F  ==  F * d(coin price)/dF
  for (OptionType type : {OptionType::Call, OptionType::Put}) {
    const double f = 86000.0, k = 88000.0, t = 0.08, vol = 0.45, h = 1.0;
    const double slope = (inverse::coin_price(type, f + h, k, t, vol) -
                          inverse::coin_price(type, f - h, k, t, vol)) /
                         (2 * h);
    EXPECT_NEAR(inverse::premium_adjusted_delta(type, f, k, t, vol), f * slope, 1e-8);
  }
}

}  // namespace

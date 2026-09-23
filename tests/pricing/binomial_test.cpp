#include "openport/pricing/binomial.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using openport::pricing::binomial_price;
using openport::pricing::bsm_price;
using openport::pricing::BsmInputs;
using openport::pricing::ExerciseStyle;
using openport::pricing::OptionType;
using openport::pricing::TreeMethod;

constexpr auto kEuropean = ExerciseStyle::European;
constexpr auto kAmerican = ExerciseStyle::American;
constexpr auto kCrr = TreeMethod::CoxRossRubinstein;
constexpr auto kLr = TreeMethod::LeisenReimer;

TEST(Binomial, EuropeanTreesConvergeToBlackScholes) {
  for (OptionType type : {OptionType::Call, OptionType::Put}) {
    for (double strike : {80.0, 100.0, 120.0}) {
      const BsmInputs in{type, 100.0, strike, 0.75, 0.04, 0.01, 0.3};
      const double exact = bsm_price(in);
      EXPECT_NEAR(binomial_price(in, kEuropean, kLr, 1001), exact, 1e-6);
      EXPECT_NEAR(binomial_price(in, kEuropean, kCrr, 2001), exact, 5e-3);
    }
  }
}

TEST(Binomial, LeisenReimerConvergesMuchFasterThanCrr) {
  const BsmInputs in{OptionType::Put, 100.0, 105.0, 1.0, 0.05, 0.0, 0.25};
  const double exact = bsm_price(in);
  const double lr_error = std::abs(binomial_price(in, kEuropean, kLr, 101) - exact);
  const double crr_error = std::abs(binomial_price(in, kEuropean, kCrr, 101) - exact);
  EXPECT_LT(lr_error * 20, crr_error);
}

TEST(Binomial, AmericanCallWithoutDividendsIsNeverExercisedEarly) {
  // With r >= 0 and no dividends a call is worth more alive than exercised, so
  // the American and European trees must agree node for node.
  const BsmInputs in{OptionType::Call, 100.0, 95.0, 1.0, 0.05, 0.0, 0.3};
  for (TreeMethod method : {kCrr, kLr}) {
    EXPECT_NEAR(binomial_price(in, kAmerican, method, 401),
                binomial_price(in, kEuropean, method, 401), 1e-12);
  }
}

TEST(Binomial, AmericanPutMatchesQuantLib) {
  // S=36, K=40, r=6%, vol=20%, T=1: the classic case from Longstaff and Schwartz (2001).
  const BsmInputs in{OptionType::Put, 36.0, 40.0, 1.0, 0.06, 0.0, 0.2};
  const double tree = binomial_price(in, kAmerican, kLr, 1001);

  // Same tree, same steps: QuantLib's BinomialVanillaEngine("lr", 1001) gives 4.48618803.
  EXPECT_NEAR(tree, 4.48618803, 1e-8);
  // The converged value is ~4.48667 (QuantLib LR at 20001 steps, FD at 8000x8000).
  // An American tree converges like 1/n, not 1/n^2, because of the exercise boundary.
  EXPECT_NEAR(tree, 4.48667, 1e-3);
  EXPECT_NEAR(bsm_price(in), 3.84431, 1e-5);  // the early-exercise premium is ~0.64
}

TEST(Binomial, AmericanCallIsExercisedEarlyAheadOfAHighDividendYield) {
  const BsmInputs in{OptionType::Call, 100.0, 80.0, 1.0, 0.01, 0.08, 0.2};
  EXPECT_GT(binomial_price(in, kAmerican, kLr, 501), binomial_price(in, kEuropean, kLr, 501) + 0.1);
}

TEST(Binomial, LeisenReimerRoundsStepsUpToOdd) {
  const BsmInputs in{OptionType::Put, 100.0, 100.0, 0.5, 0.03, 0.0, 0.2};
  EXPECT_DOUBLE_EQ(binomial_price(in, kAmerican, kLr, 100),
                   binomial_price(in, kAmerican, kLr, 101));
}

TEST(Binomial, ZeroVolAmericanRespectsImmediateExercise) {
  const BsmInputs in{OptionType::Put, 80, 100, 1, 0.1, 0, 0};
  for (auto method : {kCrr, kLr})
    EXPECT_NEAR(binomial_price(in, kAmerican, method, 101), 20, 1e-12);
}

TEST(Binomial, InadmissibleCrrProbabilitiesHaveAStableFallback) {
  for (auto type : {OptionType::Put, OptionType::Call}) {
    const BsmInputs in{type, 100, 100, 1, 0.1, 0, 0.01};
    const double p = binomial_price(in, kEuropean, kCrr, 1);
    EXPECT_TRUE(std::isfinite(p));
    EXPECT_GE(p, 0);
    EXPECT_NEAR(p, bsm_price(in), 1e-6);
  }
}

TEST(Binomial, SaturatedLeisenReimerTailsStayFinite) {
  for (auto type : {OptionType::Put, OptionType::Call}) {
    for (double s : {1.0, 10000.0}) {
      const BsmInputs in{type, s, 100, 1, 0.05, 0, 0.01};
      const double p = binomial_price(in, kEuropean, kLr, 101);
      EXPECT_TRUE(std::isfinite(p));
      EXPECT_GE(p, 0);
      EXPECT_NEAR(p, bsm_price(in), 1e-7);
      EXPECT_GE(binomial_price(in, kAmerican, kLr, 101),
                std::max(openport::pricing::omega(type) * (s - 100), 0.0));
    }
  }
}

TEST(Binomial, DeterministicAmericanCanExerciseBetweenTodayAndExpiry) {
  const BsmInputs in{OptionType::Call, 100, 90, 10, .2, .1, 0};
  const double optimal_time = std::log(.1 * 100 / (.2 * 90)) / (.1 - .2);
  const double exact = 100 * std::exp(-.1 * optimal_time) - 90 * std::exp(-.2 * optimal_time);
  EXPECT_GT(exact, std::max(10.0, bsm_price(in)));
  for (auto method : {kCrr, kLr})
    EXPECT_NEAR(binomial_price(in, kAmerican, method, 101), exact, 1e-10);
}

}  // namespace

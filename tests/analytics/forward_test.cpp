#include "openport/analytics/forward.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

#include "openport/pricing/black.hpp"

namespace {

using openport::analytics::implied_forward;
using openport::analytics::ParityPoint;
using openport::pricing::black_price;
using openport::pricing::OptionType;

// A chain priced from a known forward and discount factor, with a volatility smile.
std::vector<ParityPoint> chain(double forward, double discount, double years, double noise,
                               unsigned seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> jitter(-noise, noise);
  std::vector<ParityPoint> points;
  for (double k = forward * 0.9; k <= forward * 1.1; k += forward * 0.01) {
    const double vol = 0.2 + 0.3 * std::pow(std::log(k / forward), 2.0);
    const double call =
        black_price(OptionType::Call, forward, k, years, vol, discount) + jitter(rng);
    const double put = black_price(OptionType::Put, forward, k, years, vol, discount) + jitter(rng);
    points.push_back({k, call, put, 1.0});
  }
  return points;
}

TEST(ImpliedForward, RecoversForwardAndDiscountFromExactPrices) {
  const double years = 0.5;
  const double discount = std::exp(-0.045 * years);
  const double forward = 5000.0 * std::exp((0.045 - 0.013) * years);
  const auto estimate = implied_forward(chain(forward, discount, years, 0.0, 1), years);
  ASSERT_TRUE(estimate.ok);
  EXPECT_TRUE(estimate.fitted_discount);
  EXPECT_NEAR(estimate.forward, forward, 1e-7 * forward);
  EXPECT_NEAR(estimate.discount, discount, 1e-10);
}

TEST(ImpliedForward, StaysCloseWithNoisyMids) {
  const double years = 0.25;
  const double discount = std::exp(-0.04 * years);
  const double forward = 600.0;
  const auto estimate = implied_forward(chain(forward, discount, years, 0.02, 2), years);
  ASSERT_TRUE(estimate.ok);
  EXPECT_NEAR(estimate.forward, forward, 0.05);
  EXPECT_NEAR(estimate.discount, discount, 2e-4);
}

TEST(ImpliedForward, DropsAStaleQuote) {
  const double years = 0.1;
  const double discount = std::exp(-0.04 * years);
  auto points = chain(5000.0, discount, years, 0.0, 3);
  points[5].call_mid += 40.0;  // one wildly stale call
  const auto estimate = implied_forward(points, years);
  ASSERT_TRUE(estimate.ok);
  EXPECT_EQ(estimate.points, static_cast<int>(points.size()) - 1);
  EXPECT_NEAR(estimate.forward, 5000.0, 1e-6 * 5000.0);
}

TEST(ImpliedForward, FallsBackToAnAssumedDiscountWithTooFewStrikes) {
  const double years = 1.0 / 365.0;
  const double discount = std::exp(-0.04 * years);
  auto points = chain(5000.0, discount, years, 0.0, 4);
  points.resize(2);
  const auto estimate = implied_forward(points, years, 0.04);
  ASSERT_TRUE(estimate.ok);
  EXPECT_FALSE(estimate.fitted_discount);
  EXPECT_NEAR(estimate.forward, 5000.0, 1e-6 * 5000.0);
}

TEST(ImpliedForward, RejectsEmptyInput) {
  EXPECT_FALSE(implied_forward({}, 0.5).ok);
}

TEST(ImpliedForward, LockedOutlierCannotDominateElevenGoodPoints) {
  std::vector<ParityPoint> points;
  for (int i = 0; i < 11; ++i) points.push_back({95.0 + i, 15.0 - i, 10.0, 25.0});
  points.push_back({100.0, 60.0, 10.0, 1e8});
  for (const auto estimate : {implied_forward(points, 1.0),
                              openport::analytics::implied_forward_given_discount(points, 1.0)}) {
    ASSERT_TRUE(estimate.ok);
    EXPECT_NEAR(estimate.forward, 100.0, 1e-8);
    EXPECT_EQ(estimate.points, 11);
  }
}

TEST(ImpliedForward, LockedQuoteCannotOutweighTwoGoodQuotesInASparseExpiry) {
  const std::vector<ParityPoint> points{{95, 15, 10, 25}, {105, 5, 10, 25}, {100, 60, 10, 1e8}};
  for (const auto estimate : {implied_forward(points, 1.0),
                              openport::analytics::implied_forward_given_discount(points, 1.0)}) {
    EXPECT_TRUE(estimate.ok);
    EXPECT_NEAR(estimate.forward, 100, 1e-8);
    EXPECT_EQ(estimate.points, 2);
  }
}

}  // namespace

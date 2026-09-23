#include "openport/pricing/black.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>

namespace {

using openport::pricing::black_greeks;
using openport::pricing::black_price;
using openport::pricing::bsm_greeks;
using openport::pricing::bsm_price;
using openport::pricing::BsmInputs;
using openport::pricing::Greeks;
using openport::pricing::OptionType;

constexpr OptionType kCall = OptionType::Call;
constexpr OptionType kPut = OptionType::Put;

TEST(Black, MatchesHullWorkedExample) {
  // Hull, "Options, Futures, and Other Derivatives": S=42, K=40, r=10%, vol=20%, T=0.5.
  BsmInputs in{kCall, 42.0, 40.0, 0.5, 0.10, 0.0, 0.20};
  EXPECT_NEAR(bsm_price(in), 4.7594223929, 1e-9);
  in.type = kPut;
  EXPECT_NEAR(bsm_price(in), 0.8085993729, 1e-9);
}

TEST(Black, MatchesHaugBlack76Example) {
  // Haug, "The Complete Guide to Option Pricing Formulas": F=K=19, T=0.75, r=10%, vol=28%.
  const double discount = std::exp(-0.10 * 0.75);
  EXPECT_NEAR(black_price(kCall, 19.0, 19.0, 0.75, 0.28, discount), 1.7010507252, 1e-9);
  EXPECT_NEAR(black_price(kPut, 19.0, 19.0, 0.75, 0.28, discount), 1.7010507252, 1e-9);
}

struct Case {
  double forward, strike, expiry, vol, discount;
};

std::vector<Case> random_cases(std::size_t n, unsigned seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> log_moneyness(-0.6, 0.6);
  std::uniform_real_distribution<double> expiry(1.0 / 365.0, 3.0);
  std::uniform_real_distribution<double> vol(0.05, 1.5);
  std::uniform_real_distribution<double> rate(-0.01, 0.08);
  std::vector<Case> cases;
  for (std::size_t i = 0; i < n; ++i) {
    const double t = expiry(rng);
    cases.push_back(
        {100.0, 100.0 * std::exp(log_moneyness(rng)), t, vol(rng), std::exp(-rate(rng) * t)});
  }
  return cases;
}

TEST(Black, PutCallParityHolds) {
  for (const Case& c : random_cases(2000, 1)) {
    const double call = black_price(kCall, c.forward, c.strike, c.expiry, c.vol, c.discount);
    const double put = black_price(kPut, c.forward, c.strike, c.expiry, c.vol, c.discount);
    EXPECT_NEAR(call - put, c.discount * (c.forward - c.strike), 1e-12 * c.forward);
  }
}

TEST(Black, PriceStaysWithinNoArbitrageBounds) {
  for (const Case& c : random_cases(2000, 2)) {
    const double call = black_price(kCall, c.forward, c.strike, c.expiry, c.vol, c.discount);
    EXPECT_GE(call, c.discount * std::max(c.forward - c.strike, 0.0) - 1e-12);
    EXPECT_LE(call, c.discount * c.forward);
  }
}

TEST(Black, PriceIncreasesWithVolatility) {
  double previous = 0.0;
  for (double vol = 0.01; vol < 3.0; vol += 0.01) {
    const double price = black_price(kCall, 100.0, 120.0, 0.5, vol);
    EXPECT_GT(price, previous);
    previous = price;
  }
}

TEST(Black, ExpiredIsIntrinsicAndZeroVolIsDiscountedIntrinsic) {
  EXPECT_DOUBLE_EQ(black_price(kCall, 110.0, 100.0, 0.0, 0.3, 0.9), 10.0);
  EXPECT_DOUBLE_EQ(black_price(kPut, 110.0, 100.0, 1.0, 0.0, 0.9), 0.0);
  const Greeks g = black_greeks(kPut, 90.0, 100.0, 0.0, 0.3, 1.0);
  EXPECT_DOUBLE_EQ(g.price, 10.0);
  EXPECT_DOUBLE_EQ(g.delta, -1.0);
  EXPECT_DOUBLE_EQ(g.gamma, 0.0);
  EXPECT_DOUBLE_EQ(g.vega, 0.0);
}

// Analytic Greeks against central finite differences of the price itself.
class BlackGreeksTest : public ::testing::TestWithParam<OptionType> {};

TEST_P(BlackGreeksTest, AgreeWithFiniteDifferences) {
  const OptionType type = GetParam();
  for (const Case& c : random_cases(500, 3)) {
    const double rate = -std::log(c.discount) / c.expiry;
    auto price = [&](double f, double vol, double t, double r) {
      return black_price(type, f, c.strike, t, vol, std::exp(-r * t));
    };
    const Greeks g = black_greeks(type, c.forward, c.strike, c.expiry, c.vol, c.discount);
    // Bump the forward in units of its own standard deviation: a fixed bump is far
    // too coarse for a one-day option, whose delta turns over within a few ticks.
    const double sd = c.vol * std::sqrt(c.expiry);
    const double hf = 1e-4 * c.forward * sd;
    const double hg = 1e-3 * c.forward * sd;
    const double hv = 1e-5;
    const double ht = 1e-6;
    const double hr = 1e-6;
    const double f = c.forward;
    const double v = c.vol;
    const double t = c.expiry;

    const double delta = (price(f + hf, v, t, rate) - price(f - hf, v, t, rate)) / (2 * hf);
    const double gamma =
        (price(f + hg, v, t, rate) - 2 * price(f, v, t, rate) + price(f - hg, v, t, rate)) /
        (hg * hg);
    const double vega = (price(f, v + hv, t, rate) - price(f, v - hv, t, rate)) / (2 * hv);
    const double theta = -(price(f, v, t + ht, rate) - price(f, v, t - ht, rate)) / (2 * ht);
    const double rho = (price(f, v, t, rate + hr) - price(f, v, t, rate - hr)) / (2 * hr);
    const double vanna = (black_greeks(type, f, c.strike, t, v + hv, c.discount).delta -
                          black_greeks(type, f, c.strike, t, v - hv, c.discount).delta) /
                         (2 * hv);
    const double volga = (black_greeks(type, f, c.strike, t, v + hv, c.discount).vega -
                          black_greeks(type, f, c.strike, t, v - hv, c.discount).vega) /
                         (2 * hv);

    EXPECT_NEAR(g.delta, delta, 1e-7);
    EXPECT_NEAR(g.gamma, gamma, 1e-5 * std::max(1.0, std::abs(g.gamma)));
    EXPECT_NEAR(g.vega, vega, 1e-6 * std::max(1.0, g.vega));
    EXPECT_NEAR(g.theta, theta, 1e-5 * std::max(1.0, std::abs(g.theta)));
    EXPECT_NEAR(g.rho, rho, 1e-6 * std::max(1.0, std::abs(g.rho)));
    EXPECT_NEAR(g.vanna, vanna, 1e-6 * std::max(1.0, std::abs(g.vanna)));
    EXPECT_NEAR(g.volga, volga, 1e-5 * std::max(1.0, std::abs(g.volga)));
  }
}

INSTANTIATE_TEST_SUITE_P(CallsAndPuts, BlackGreeksTest, ::testing::Values(kCall, kPut));

class BsmGreeksTest : public ::testing::TestWithParam<OptionType> {};

TEST_P(BsmGreeksTest, AgreeWithFiniteDifferences) {
  const OptionType type = GetParam();
  std::mt19937_64 rng(4);
  std::uniform_real_distribution<double> strike(70.0, 140.0);
  std::uniform_real_distribution<double> expiry(0.02, 2.0);
  std::uniform_real_distribution<double> vol(0.1, 0.9);
  std::uniform_real_distribution<double> rate(0.0, 0.06);
  std::uniform_real_distribution<double> dividend(0.0, 0.04);
  for (int i = 0; i < 500; ++i) {
    const BsmInputs in{type, 100.0, strike(rng), expiry(rng), rate(rng), dividend(rng), vol(rng)};
    const Greeks g = bsm_greeks(in);
    EXPECT_NEAR(g.price, bsm_price(in), 1e-12);

    auto bumped = [&](auto member, double h) {
      BsmInputs up = in;
      BsmInputs down = in;
      up.*member += h;
      down.*member -= h;
      return (bsm_price(up) - bsm_price(down)) / (2 * h);
    };
    EXPECT_NEAR(g.delta, bumped(&BsmInputs::spot, 1e-3), 1e-7);
    EXPECT_NEAR(g.vega, bumped(&BsmInputs::vol, 1e-5), 1e-5);
    EXPECT_NEAR(g.theta, -bumped(&BsmInputs::expiry, 1e-6),
                1e-4 * std::max(1.0, std::abs(g.theta)));
    EXPECT_NEAR(g.rho, bumped(&BsmInputs::rate, 1e-6), 1e-5);

    BsmInputs up = in;
    BsmInputs down = in;
    up.spot += 1e-2;
    down.spot -= 1e-2;
    EXPECT_NEAR(g.gamma, (bsm_greeks(up).delta - bsm_greeks(down).delta) / 2e-2, 1e-7);
  }
}

INSTANTIATE_TEST_SUITE_P(CallsAndPuts, BsmGreeksTest, ::testing::Values(kCall, kPut));

TEST(Black, NegativeExpiryMatchesExpiryForPriceAndGreeks) {
  for (auto type : {kCall, kPut}) {
    BsmInputs in{type, 80, 100, -1, 0.1, 0.02, 0.3};
    const double intrinsic = type == kPut ? 20 : 0;
    EXPECT_DOUBLE_EQ(bsm_price(in), intrinsic);
    EXPECT_DOUBLE_EQ(bsm_greeks(in).price, intrinsic);
    for (double t : {-1.0, 0.0}) {
      EXPECT_DOUBLE_EQ(black_price(type, 80, 100, t, 0.3, 0.9), intrinsic);
      const auto g = black_greeks(type, 80, 100, t, 0.3, 0.9);
      EXPECT_DOUBLE_EQ(g.price, intrinsic);
      EXPECT_DOUBLE_EQ(g.theta, 0);
      EXPECT_DOUBLE_EQ(g.rho, 0);
    }
  }
}

TEST(Black, ZeroVolThetaAndRhoMatchDeterministicFiniteDifferences) {
  for (auto type : {kCall, kPut}) {
    const double s = type == kCall ? 120 : 80;
    BsmInputs in{type, s, 100, 1, 0.1, 0.02, 0};
    auto up = in, down = in;
    constexpr double h = 1e-6;
    up.expiry += h;
    down.expiry -= h;
    EXPECT_NEAR(bsm_greeks(in).theta, -(bsm_price(up) - bsm_price(down)) / (2 * h), 1e-7);
    up = down = in;
    up.rate += h;
    down.rate -= h;
    EXPECT_NEAR(bsm_greeks(in).rho, (bsm_price(up) - bsm_price(down)) / (2 * h), 1e-7);
    const auto g = black_greeks(type, s, 100, 1, 0, std::exp(-0.1));
    EXPECT_NEAR(g.theta, 0.1 * g.price, 1e-12);
    EXPECT_NEAR(g.rho, -g.price, 1e-12);
  }
}

}  // namespace

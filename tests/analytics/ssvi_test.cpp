#include "openport/analytics/ssvi.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace {
using namespace openport::analytics;
const SsviParameters known{-.65, .9, .3};

std::vector<SsviSlice> surface(const SsviParameters& p = known, double noise = 0) {
  std::vector<SsviSlice> slices;
  for (double years : {.01, .05, .2, .7, 1.5, 3.0}) {
    SsviSlice slice{years, {}};
    for (int i = -30; i <= 30; ++i) {
      const double k = .02 * i;
      const double iv = ssvi_iv(p, k, .04 * years, years) + noise * std::sin(i * 2.3);
      slice.points.push_back({k, iv, iv - .002, iv + .002});
    }
    slices.push_back(slice);
  }
  return slices;
}

TEST(Ssvi, RecoversNoiseFreeAndNoisySurfaces) {
  for (const auto& p : {known, SsviParameters{.45, .6, .5}, SsviParameters{0, .8, .12}}) {
    for (double noise : {0.0, .0001}) {
      const auto fit = fit_ssvi(surface(p, noise));
      ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
      EXPECT_NEAR(fit.parameters.rho, p.rho, noise == 0 ? 1e-6 : .002);
      EXPECT_NEAR(fit.parameters.eta, p.eta, noise == 0 ? 1e-6 : .002);
      EXPECT_NEAR(fit.parameters.gamma, p.gamma, noise == 0 ? 1e-6 : .002);
      EXPECT_LT(fit.rmse_vol_points, noise == 0 ? 1e-6 : .011);
      EXPECT_FALSE(fit.monotone_adjusted);
      EXPECT_GT(fit.fit_ms, 0);
      ASSERT_EQ(fit.expiries.size(), 6u);
      for (const auto& expiry : fit.expiries) {
        EXPECT_GT(expiry.theta, 0);
        EXPECT_LT(expiry.rmse_vol_points, noise == 0 ? 1e-6 : .011);
        EXPECT_EQ(expiry.points, 61u);
      }
    }
  }
}

TEST(Ssvi, FittedSurfacePassesExistingDensityAndCalendarChecksBeyondQuotes) {
  const auto input = surface(known, .0002);
  const auto fit = fit_ssvi(input);
  ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
  ASSERT_TRUE(ssvi_admissible(fit.parameters));
  std::vector<SviFit> slices;
  for (std::size_t i = 0; i < input.size(); ++i) {
    SviFit slice;
    slice.parameters = ssvi_slice(fit.parameters, fit.expiries[i].theta);
    slice.years = input[i].years;
    slice.status = SviStatus::Ok;
    slice.min_k = -20; slice.max_k = 20; slice.rmse_vol_points = 0;
    EXPECT_TRUE(svi_admissible(slice.parameters, slice.years));
    EXPECT_TRUE(svi_butterfly(slice.parameters, -20, 20).ok);
    for (int j = 0; j <= 2000; ++j) {
      const double k = -20 + .02 * j;
      EXPECT_NEAR(svi_variance(slice.parameters, k), ssvi_variance(fit.parameters, k, fit.expiries[i].theta), 1e-12);
      if (i > 0) {
        EXPECT_GE(svi_variance(slice.parameters, k) + 1e-13, svi_variance(slices.back().parameters, k));
      }
    }
    slices.push_back(slice);
  }
  EXPECT_TRUE(svi_calendar(slices).empty());
}

TEST(Ssvi, BoundaryConstraintsStaySafeAcrossThetaAndBothWings) {
  for (double rho : {-.999, -.5, 0.0, .5, .999}) {
    for (double gamma : {1e-6, .25, .5}) {
      const SsviParameters p{rho, 2 / (1 + std::abs(rho)), gamma};
      EXPECT_TRUE(ssvi_admissible(p));
      double previous = 0;
      for (double theta : {1e-8, 1e-5, .001, .1, 1.0, 100.0}) {
        const auto raw = ssvi_slice(p, theta);
        EXPECT_TRUE(svi_butterfly(raw, -100, 100).ok);
        for (double k : {-100.0, -1.0, 0.0, 1.0, 100.0}) {
          const double w = ssvi_variance(p, k, theta);
          EXPECT_GT(w, 0);
          if (previous > 0) {
            EXPECT_GE(w, ssvi_variance(p, k, previous));
          }
        }
        previous = theta;
      }
    }
  }
  EXPECT_FALSE(ssvi_admissible({1, .1, .3}));
  EXPECT_FALSE(ssvi_admissible({0, 0, .3}));
  EXPECT_FALSE(ssvi_admissible({0, .1, 0}));
  EXPECT_FALSE(ssvi_admissible({0, .1, .51}));
  EXPECT_FALSE(ssvi_admissible({-.5, 1.34, .3}));
  EXPECT_TRUE(std::isnan(ssvi_iv(known, 0, .04, 0)));
}

TEST(Ssvi, RepairsThetaWithPoolAdjacentViolatorsAndPoolsEqualTenors) {
  auto input = surface();
  const std::vector<double> targets{.01, .03, .02, .01, .08, .12};
  for (std::size_t i = 0; i < input.size(); ++i) {
    for (auto& point : input[i].points) {
      point.iv = ssvi_iv(known, point.k, targets[i], input[i].years);
      point.bid_iv = point.iv - .002; point.ask_iv = point.iv + .002;
    }
  }
  const auto fit = fit_ssvi(input);
  ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
  EXPECT_TRUE(fit.monotone_adjusted);
  EXPECT_NEAR(fit.expiries[0].theta, .01, 1e-14);
  for (std::size_t i = 1; i < 4; ++i) EXPECT_NEAR(fit.expiries[i].theta, .02, 1e-14);
  input[1].years = input[0].years;
  const auto tied = fit_ssvi(input);
  ASSERT_EQ(tied.status, SviStatus::Ok);
  EXPECT_TRUE(tied.monotone_adjusted);
  EXPECT_DOUBLE_EQ(tied.expiries[0].theta, tied.expiries[1].theta);
  for (std::size_t i = 1; i < input.size(); ++i) EXPECT_LE(tied.expiries[i - 1].theta, tied.expiries[i].theta);
}

TEST(Ssvi, InterpolatesAtmIvAndDoesNotExtrapolateMissingAtmQuotes) {
  auto input = surface();
  input[0].points.erase(input[0].points.begin() + 30);  // No exact ATM observation.
  const double atm = (input[0].points[29].iv + input[0].points[30].iv) / 2;
  input[1].points.erase(input[1].points.begin(), input[1].points.begin() + 31);
  input[2].years = kNaN;
  input[3].points.resize(4);
  const auto fit = fit_ssvi(input);
  ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
  EXPECT_NEAR(fit.expiries[0].theta, atm * atm * input[0].years, 1e-14);
  for (std::size_t i = 1; i < 4; ++i) {
    EXPECT_TRUE(std::isnan(fit.expiries[i].theta));
    EXPECT_FALSE(fit.expiries[i].reason.empty());
  }
  input.resize(1);
  EXPECT_EQ(fit_ssvi(input).status, SviStatus::TooFewPoints);
  EXPECT_EQ(fit_ssvi(std::vector<SsviSlice>{}).status, SviStatus::TooFewPoints);
}

TEST(Ssvi, DeterministicIncludingReorderedQuotesAndTenors) {
  auto input = surface(known, .0001);
  const auto a = fit_ssvi(input);
  for (auto& slice : input) std::reverse(slice.points.begin(), slice.points.end());
  std::reverse(input.begin(), input.end());
  const auto b = fit_ssvi(input);
  ASSERT_EQ(a.status, SviStatus::Ok);
  ASSERT_EQ(b.status, SviStatus::Ok);
  EXPECT_DOUBLE_EQ(a.parameters.rho, b.parameters.rho);
  EXPECT_DOUBLE_EQ(a.parameters.eta, b.parameters.eta);
  EXPECT_DOUBLE_EQ(a.parameters.gamma, b.parameters.gamma);
  EXPECT_DOUBLE_EQ(a.rmse_vol_points, b.rmse_vol_points);
  for (std::size_t i = 0; i < input.size(); ++i)
    EXPECT_DOUBLE_EQ(a.expiries[i].theta, b.expiries[input.size() - 1 - i].theta);
}

TEST(Ssvi, WideOutlierQuotesReceiveTheSameDownweightingAsSvi) {
  auto input = surface();
  for (auto& slice : input) {
    slice.points.front().iv += .05;
    slice.points.front().bid_iv = slice.points.front().iv - .1;
    slice.points.front().ask_iv = slice.points.front().iv + .1;
  }
  const auto fit = fit_ssvi(input);
  ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
  EXPECT_NEAR(fit.parameters.rho, known.rho, .001);
  EXPECT_NEAR(fit.parameters.eta, known.eta, .001);
  EXPECT_NEAR(fit.parameters.gamma, known.gamma, .001);
}
}  // namespace

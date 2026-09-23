#include "openport/analytics/svi.hpp"
#include "openport/analytics/ssvi.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace {
using namespace openport::analytics;
const SviParameters known{.025, .12, -.4, .03, .18};

std::vector<SviPoint> smile(const SviParameters& p = known, double years = .7, double noise = 0) {
  std::vector<SviPoint> points;
  for (int i = 0; i <= 60; ++i) {
    const double k = -.6 + .02 * i;
    const double iv = svi_iv(p, k, years) + noise * std::sin(i * 2.3);
    points.push_back({k, iv, iv - .002, iv + .002});
  }
  return points;
}

TEST(Svi, RecoversAllFiveParametersIncludingNegativeA) {
  for (const auto& p : {known, SviParameters{-.041, .1331, .306, .3586, .4153}}) {
    const auto fit = fit_svi(smile(p), .7);
    ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
    EXPECT_NEAR(fit.parameters.a, p.a, 1e-6);
    EXPECT_NEAR(fit.parameters.b, p.b, 1e-6);
    EXPECT_NEAR(fit.parameters.rho, p.rho, 1e-5);
    EXPECT_NEAR(fit.parameters.m, p.m, 1e-6);
    EXPECT_NEAR(fit.parameters.sigma, p.sigma, 1e-5);
    EXPECT_LT(fit.rmse_vol_points, 1e-6);
    EXPECT_EQ(fit.points, 61u);
    EXPECT_GT(fit.fit_ms, 0);
  }
}

TEST(Svi, SmallNoiseRecoversSmileAndParameters) {
  const auto fit = fit_svi(smile(known, .7, .0001), .7);
  ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
  EXPECT_NEAR(fit.parameters.a, known.a, .001);
  EXPECT_NEAR(fit.parameters.b, known.b, .001);
  EXPECT_NEAR(fit.parameters.rho, known.rho, .005);
  EXPECT_NEAR(fit.parameters.m, known.m, .002);
  EXPECT_NEAR(fit.parameters.sigma, known.sigma, .003);
  EXPECT_LT(fit.rmse_vol_points, .011);
}

TEST(Svi, DeterministicAndInsensitiveToInputOrder) {
  auto points = smile();
  const auto a = fit_svi(points, .7);
  std::reverse(points.begin(), points.end());
  const auto b = fit_svi(points, .7);
  ASSERT_EQ(a.status, SviStatus::Ok);
  EXPECT_EQ(a.status, b.status);
  EXPECT_DOUBLE_EQ(a.parameters.a, b.parameters.a);
  EXPECT_DOUBLE_EQ(a.parameters.b, b.parameters.b);
  EXPECT_DOUBLE_EQ(a.parameters.rho, b.parameters.rho);
  EXPECT_DOUBLE_EQ(a.parameters.m, b.parameters.m);
  EXPECT_DOUBLE_EQ(a.parameters.sigma, b.parameters.sigma);
  EXPECT_DOUBLE_EQ(a.rmse_vol_points, b.rmse_vol_points);
}

TEST(Svi, FailsClosedOnMissingInvalidAndDegenerateData) {
  auto points = smile();
  for (std::size_t i = 4; i < points.size(); ++i) points[i].ask_iv = kNaN;
  auto fit = fit_svi(points, .7);
  EXPECT_EQ(fit.status, SviStatus::TooFewPoints);
  EXPECT_EQ(fit.points, 4u);
  EXPECT_FALSE(fit.reason.empty());
  EXPECT_TRUE(std::isnan(fit.parameters.a));
  EXPECT_EQ(fit_svi(smile(), 0).status, SviStatus::Failed);
  EXPECT_EQ(fit_svi(smile(), kNaN).status, SviStatus::Failed);
  EXPECT_EQ(fit_svi(std::vector<SviPoint>(8, {.1, .2, .19, .21}), .7).status,
            SviStatus::TooFewPoints);
  points = smile();
  for (auto& p : points) p.k *= 1e-8;
  EXPECT_EQ(fit_svi(points, .7).status, SviStatus::Failed);
  points = smile();
  points[0].bid_iv = 0;
  points[1].ask_iv = points[1].bid_iv - .01;
  points[2].iv = kNaN;
  EXPECT_EQ(fit_svi(points, .7).points, 58u);
}

TEST(Svi, WeightsFavorTightQuotesButOneQuoteCannotDominate) {
  auto points = smile();
  points[30].iv += .05;
  points[30].bid_iv = points[30].iv - .1;
  points[30].ask_iv = points[30].iv + .1;
  const auto wide = fit_svi(points, .7);
  points[30].bid_iv = points[30].iv - 1e-9;
  points[30].ask_iv = points[30].iv + 1e-9;
  const auto tight = fit_svi(points, .7);
  ASSERT_EQ(wide.status, SviStatus::Ok) << wide.reason;
  ASSERT_EQ(tight.status, SviStatus::Ok) << tight.reason;
  EXPECT_LT(std::abs(svi_iv(wide.parameters, 0, .7) - svi_iv(known, 0, .7)), .001);
  EXPECT_GT(svi_iv(tight.parameters, 0, .7), svi_iv(wide.parameters, 0, .7));
  EXPECT_LT(svi_iv(tight.parameters, 0, .7) - svi_iv(known, 0, .7), .02);
}

TEST(Svi, EnforcesConstraintsAndAcceptsFlatSmile) {
  EXPECT_TRUE(svi_admissible(known, .7));
  EXPECT_FALSE(svi_admissible({.1, -.1, 0, 0, .1}, 1));
  EXPECT_FALSE(svi_admissible({.1, .1, 1, 0, .1}, 1));
  EXPECT_FALSE(svi_admissible({.1, .1, 0, 0, 0}, 1));
  EXPECT_FALSE(svi_admissible({-.1, .1, 0, 0, .1}, 1));
  EXPECT_TRUE(svi_admissible({.1, 2, 0, 0, .1}, 1));
  EXPECT_FALSE(svi_admissible({.1, 2.01, 0, 0, .1}, 1));
  EXPECT_TRUE(svi_admissible({.1, 1, 0, 0, .1}, 4));
  EXPECT_TRUE(svi_admissible({.1, 1.8, 0, 0, .1}, 4));
  EXPECT_FALSE(svi_admissible({.1, 2.01, 0, 0, .1}, 4));
  EXPECT_FALSE(svi_admissible({.1, 2, 0, 0, 1e308}, 1));
  for (const auto& p : {SviParameters{.04, 0, 0, 0, .1},
                        SviParameters{.1, 2.2, .5, 0, .1},
                        SviParameters{-.019, .1, 0, 0, .2},
                        SviParameters{.01, .1, .999999, 0, .1}}) {
    const auto fit = fit_svi(smile(p, 1), 1);
    ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
    EXPECT_TRUE(svi_admissible(fit.parameters, 1));
    EXPECT_GE(fit.parameters.a + fit.parameters.b * fit.parameters.sigma *
              std::sqrt(1 - fit.parameters.rho * fit.parameters.rho), -1e-12);
  }
}

TEST(Svi, ActiveMinimumVarianceConstraintAndLongTenorWingCap) {
  // The unconstrained target is negative between quotes near its minimum;
  // retain only positive, two-sided observations and require a feasible result.
  const SviParameters invalid{-.021, .1, 0, .013, .2};
  const auto fit = fit_svi(smile(invalid, 1), 1);
  ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
  EXPECT_TRUE(svi_admissible(fit.parameters, 1));
  EXPECT_GT(fit.rmse_vol_points, .001);
  const auto long_fit = fit_svi(smile({.1, 1.2, .5, 0, .1}, 3), 3);
  ASSERT_EQ(long_fit.status, SviStatus::Ok) << long_fit.reason;
  EXPECT_NEAR(long_fit.parameters.b * (1 + std::abs(long_fit.parameters.rho)), 1.8, 1e-6);
  EXPECT_LT(long_fit.rmse_vol_points, 1e-6);
}

TEST(Svi, RecoversAcrossTenorsAndRetainsButterflyViolations) {
  for (double years : {.001, .02, .5, 2.0}) {
    auto p = known;
    p.a *= years; p.b *= years;
    const auto fit = fit_svi(smile(p, years), years);
    ASSERT_EQ(fit.status, SviStatus::Ok) << years << ": " << fit.reason;
    EXPECT_LT(fit.rmse_vol_points, 1e-6);
    EXPECT_NEAR(fit.parameters.rho, p.rho, 1e-5);
  }
  const SviParameters vogt{-.041, .1331, .306, .3586, .4153};
  std::vector<SviPoint> points;
  for (int i = 0; i <= 100; ++i) {
    const double k = -2 + .04 * i;
    const double iv = svi_iv(vogt, k, 1);
    points.push_back({k, iv, iv - .001, iv + .001});
  }
  const auto fit = fit_svi(points, 1);
  ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
  EXPECT_FALSE(fit.butterfly.ok);
  EXPECT_LT(fit.butterfly.min_g, 0);
  EXPECT_NEAR(fit.parameters.a, vogt.a, 1e-6);
}

TEST(Svi, DensityMatchesFlatSmileAndDetectsVogtButterfly) {
  const SviParameters flat{.04, 0, 0, 0, .2};
  EXPECT_DOUBLE_EQ(svi_density(flat, 1), 1);
  EXPECT_TRUE(svi_butterfly(flat, -2, 2).ok);
  EXPECT_TRUE(svi_butterfly(known, -.6, .6).ok);
  const SviParameters vogt{-.041, .1331, .306, .3586, .4153};
  const auto butterfly = svi_butterfly(vogt, -2, 2);
  EXPECT_FALSE(butterfly.ok);
  EXPECT_LT(butterfly.min_g, -.01);
  EXPECT_NEAR(butterfly.min_g, svi_density(vogt, butterfly.k), 1e-12);
  EXPECT_FALSE(svi_butterfly({0, 0, 0, 0, .1}, -1, 1).ok);
  EXPECT_TRUE(std::isnan(svi_density({0, 0, 0, 0, .1}, 0)));
}

TEST(Svi, CalendarSortsTenorsSkipsFailuresAndReturnsWorstPairLocation) {
  SviFit early, late;
  early.status = late.status = SviStatus::Ok;
  early.parameters = {.04, 0, 0, 0, .1};
  late.parameters = {.05, 0, 0, 0, .1};
  early.years = .5; late.years = 1;
  early.min_k = late.min_k = -.5;
  early.max_k = late.max_k = .5;
  EXPECT_TRUE(svi_calendar(std::vector<SviFit>{late, {}, early}).empty());
  late.parameters.a = .03;
  const auto violations = svi_calendar(std::vector<SviFit>{late, {}, early});
  ASSERT_EQ(violations.size(), 1u);
  EXPECT_EQ(violations[0].earlier, 2u);
  EXPECT_EQ(violations[0].later, 0u);
  EXPECT_GE(violations[0].k, -.5);
  EXPECT_LE(violations[0].k, .5);
  EXPECT_NEAR(violations[0].vol_points, 100 * (.2 - std::sqrt(.03)), 1e-12);
}

TEST(Svi, CalendarIgnoresExtrapolatedCrossingsAndDisjointRanges) {
  SviFit early, late;
  early.status = late.status = SviStatus::Ok;
  early.parameters = {.03, .1, 0, 0, .05};
  late.parameters = {.05, 0, 0, 0, .1};
  early.years = .5; late.years = 1;
  early.min_k = -.5; early.max_k = .1;
  late.min_k = -.1; late.max_k = .5;
  ASSERT_GT(svi_variance(early.parameters, -.5), svi_variance(late.parameters, -.5));
  ASSERT_GT(svi_variance(early.parameters, .5), svi_variance(late.parameters, .5));
  EXPECT_TRUE(svi_calendar(std::vector<SviFit>{early, late}).empty());
  early.max_k = -.2;
  late.min_k = .2;
  EXPECT_TRUE(svi_calendar(std::vector<SviFit>{early, late}).empty());
}

TEST(Svi, CalendarReportsLaterExpiryVolIncreaseInsideIntersection) {
  SviFit early, late;
  early.status = late.status = SviStatus::Ok;
  early.parameters = {.03, .1, .5, 0, .05};
  late.parameters = {.04, 0, 0, 0, .1};
  early.years = 6.0 / 365; late.years = 7.0 / 365;
  early.min_k = -.5; early.max_k = .1;
  late.min_k = -.1; late.max_k = .5;
  const auto violations = svi_calendar(std::vector<SviFit>{early, late});
  ASSERT_EQ(violations.size(), 1u);
  EXPECT_NEAR(violations[0].k, .1, 1e-12);
  const double expected = 100 * (std::sqrt(svi_variance(early.parameters, .1) / late.years) -
                                  std::sqrt(.04 / late.years));
  EXPECT_NEAR(violations[0].vol_points, expected, 1e-12);
  const double repaired_iv = svi_iv(late.parameters, .1, late.years) + violations[0].vol_points / 100;
  EXPECT_NEAR(repaired_iv * repaired_iv * late.years, svi_variance(early.parameters, .1), 1e-12);
}

TEST(Svi, CalendarToleranceUsesFloorAndLargerFitRmseAtLaterExpiry) {
  SviFit early, late;
  early.status = late.status = SviStatus::Ok;
  early.years = 6.0 / 365; late.years = 7.0 / 365;
  early.min_k = late.min_k = -.1;
  early.max_k = late.max_k = .1;
  late.parameters = {.2 * .2 * late.years, 0, 0, 0, .1};
  for (const auto& rmses : {std::pair{0.0, 0.0}, std::pair{.5, .2}, std::pair{.2, .5}}) {
    early.rmse_vol_points = rmses.first;
    late.rmse_vol_points = rmses.second;
    const double tolerance = std::max({.1, rmses.first, rmses.second});
    for (const double scale : {.99, 1.0, 1.01}) {
      const double iv = .2 + scale * tolerance / 100;
      early.parameters = {iv * iv * late.years, 0, 0, 0, .1};
      const auto violations = svi_calendar(std::vector<SviFit>{early, late});
      if (scale <= 1) {
        EXPECT_TRUE(violations.empty());
      } else {
        ASSERT_EQ(violations.size(), 1u);
        EXPECT_NEAR(violations[0].vol_points, scale * tolerance, 1e-12);
      }
    }
  }
}

TEST(Svi, CalendarUsesInterpolatedLocalHalfSpreadFromWiderExpiry) {
  SviFit early, late;
  early.status = late.status = SviStatus::Ok;
  early.years = .5; late.years = 1;
  early.min_k = late.min_k = 0;
  early.max_k = late.max_k = 0;  // Probe the midpoint of quoted spreads.
  late.parameters = {.04, 0, 0, 0, .1};
  early.half_spreads = {{-.1, .2}, {.1, .8}};
  late.half_spreads = {{-.1, .1}, {.1, .3}};
  for (double increase : {.49, .5, .51}) {
    early.parameters = {std::pow(.2 + increase / 100, 2), 0, 0, 0, .1};
    const auto violations = svi_calendar(std::vector<SviFit>{early, late});
    if (increase <= .5) {
      EXPECT_TRUE(violations.empty());
    } else {
      ASSERT_EQ(violations.size(), 1u);
      EXPECT_NEAR(violations[0].vol_points, increase, 1e-12);
      EXPECT_DOUBLE_EQ(violations[0].tolerance_vol_points, .5);
    }
  }
  std::swap(early.half_spreads, late.half_spreads);
  const auto violations = svi_calendar(std::vector<SviFit>{early, late});
  ASSERT_EQ(violations.size(), 1u);
  EXPECT_DOUBLE_EQ(violations[0].tolerance_vol_points, .5);
}

TEST(Svi, CalendarFindsWingCrossingEvenWhenAtmVarianceIncreases) {
  SviFit early, late;
  early.status = late.status = SviStatus::Ok;
  early.parameters = {.03, .1, 0, 0, .05};
  late.parameters = {.05, 0, 0, 0, .1};
  early.years = .5; late.years = 1;
  early.min_k = late.min_k = -.5;
  early.max_k = late.max_k = .5;
  ASSERT_GT(svi_variance(late.parameters, 0), svi_variance(early.parameters, 0));
  const auto violations = svi_calendar(std::vector<SviFit>{early, late});
  ASSERT_EQ(violations.size(), 1u);
  EXPECT_DOUBLE_EQ(std::abs(violations[0].k), .5);
}

TEST(Svi, SliceUsesOnlyTheOtmSidesTwoSidedMarket) {
  SliceMetrics slice;
  slice.years = .7;
  slice.forward.forward = 5000;
  for (const auto& p : smile()) {
    StrikeMetrics row;
    row.strike = 5000 * std::exp(p.k);
    row.iv = p.iv;
    auto& side = p.k >= 0 ? row.call : row.put;
    side.bid = 1; side.ask = 1.1;
    side.bid_iv = p.bid_iv; side.ask_iv = p.ask_iv;
    slice.strikes.push_back(row);
  }
  slice.strikes[0].put.bid = 0;
  slice.strikes[1].put.ask = kNaN;
  slice.strikes.back().call.ask = .9;
  const auto fit = fit_svi(slice);
  ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
  EXPECT_EQ(fit.points, 58u);
  ASSERT_EQ(fit.half_spreads.size(), 58u);
  EXPECT_NEAR(fit.half_spreads.front().vol_points, .2, 1e-12);
  EXPECT_LT(fit.rmse_vol_points, 1e-6);
  slice.forward.forward = kNaN;
  EXPECT_EQ(fit_svi(slice).status, SviStatus::Failed);
}
}  // namespace

#ifdef OPENPORT_APPS_DIR
#include <future>
#include <nlohmann/json.hpp>

#include "openport/server/api.hpp"

namespace {
using namespace openport;
using nlohmann::json;

class SviSource final : public server::MetricsSource {
 public:
  std::shared_ptr<const analytics::UnderlyingMetrics> snapshot;
  std::vector<std::string> symbols() const override { return {"SPX"}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override {
    return snapshot;
  }
  server::EngineStatus status() const override { return {}; }
};

analytics::UnderlyingMetrics surface_fixture() {
  analytics::UnderlyingMetrics m;
  m.symbol = "SPX"; m.version = 123; m.spot = 5000;
  for (int i = 0; i < 3; ++i) {
    analytics::SliceMetrics slice;
    slice.expiry = {2027, 1, 15 + i};
    slice.expiry_time = md::new_york_to_utc(slice.expiry, 16, 0);
    slice.years = .7 + .1 * i;
    slice.forward.forward = 5000;
    slice.forward.discount = 1;
    auto p = known;
    p.a -= .005 * i;  // Intentional calendar violation in total variance.
    for (const auto& point : smile(p, slice.years)) {
      analytics::StrikeMetrics row;
      row.strike = 5000 * std::exp(point.k);
      row.iv = point.iv;
      auto& otm = point.k >= 0 ? row.call : row.put;
      otm.bid = 1; otm.ask = 1.1; otm.mid = 1.05;
      otm.bid_iv = point.bid_iv; otm.ask_iv = point.ask_iv;
      slice.strikes.push_back(row);
    }
    if (i == 2) slice.strikes.resize(4);
    m.slices.push_back(slice);
  }
  return m;
}

json surface_get(const SviSource& source, const std::string& query = "") {
  const auto r = server::handle_api({"GET", "/api/underlyings/SPX/surface" + query}, source);
  EXPECT_EQ(r.status, 200);
  return json::parse(r.body);
}

TEST(SviApi, PreservesFieldsAndReportsParametersNullFailuresAndCalendarPairs) {
  SviSource source;
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(surface_fixture());
  const auto response = surface_get(source, "?window=0");
  for (const auto key : {"symbol", "spot", "spot_source", "as_of", "version", "expiries"})
    EXPECT_TRUE(response.contains(key));
  const auto& ssvi = response["ssvi"];
  EXPECT_EQ(ssvi["status"], "ok");
  EXPECT_TRUE(ssvi["reason"].is_null());
  EXPECT_TRUE(ssvi["monotone_adjusted"].get<bool>());
  for (const auto* key : {"rho", "eta", "gamma", "rmse_vol_points", "fit_ms"})
    EXPECT_TRUE(ssvi[key].is_number()) << key;
  const SsviParameters parameters{ssvi["rho"].get<double>(), ssvi["eta"].get<double>(), ssvi["gamma"].get<double>()};
  EXPECT_TRUE(ssvi_admissible(parameters));
  const auto& e = response["expiries"][0];
  EXPECT_TRUE(e["ssvi_theta"].is_number());
  EXPECT_TRUE(e["ssvi_rmse_vol_points"].is_number());
  EXPECT_TRUE(e["ssvi_reason"].is_null());
  EXPECT_EQ(e["ssvi_theta"], response["expiries"][1]["ssvi_theta"]);
  for (const auto key : {"id", "expiry", "days", "forward", "atm_iv", "points"})
    EXPECT_TRUE(e.contains(key));
  const auto& fit = e["svi"];
  EXPECT_EQ(fit["status"], "ok");
  EXPECT_TRUE(fit["reason"].is_null());
  EXPECT_EQ(fit["points"], 61);
  for (const auto key : {"a", "b", "rho", "m", "sigma", "rmse_vol_points", "fit_ms", "butterfly_min_g", "butterfly_k"})
    EXPECT_TRUE(fit[key].is_number()) << key;
  EXPECT_TRUE(fit["butterfly_ok"].is_boolean());
  for (const auto& p : e["points"]) {
    for (const auto key : {"strike", "k", "iv", "bid_iv", "ask_iv", "svi_iv"})
      EXPECT_TRUE(p[key].is_number()) << key;
    EXPECT_NEAR(p["svi_iv"].get<double>(), p["iv"].get<double>(), 1e-6);
    EXPECT_NEAR(p["ssvi_iv"].get<double>(), ssvi_iv(parameters, p["k"].get<double>(),
        e["ssvi_theta"].get<double>(), e["svi_years"].get<double>()), 1e-6);
  }
  const auto& failure = response["expiries"][2];
  EXPECT_TRUE(failure["svi"].is_null());
  EXPECT_EQ(failure["svi_status"], "too_few_points");
  EXPECT_TRUE(failure["svi_reason"].is_string());
  EXPECT_EQ(failure["svi_points"], 4);
  EXPECT_TRUE(failure["ssvi_theta"].is_null());
  EXPECT_TRUE(failure["ssvi_rmse_vol_points"].is_null());
  EXPECT_TRUE(failure["ssvi_reason"].is_string());
  for (const auto& p : failure["points"]) {
    EXPECT_TRUE(p["svi_iv"].is_null());
    EXPECT_TRUE(p["ssvi_iv"].is_null());
  }
  const auto& pairs = response["calendar_violations"];
  ASSERT_EQ(pairs.size(), 1u);
  EXPECT_EQ(pairs[0]["earlier"], response["expiries"][0]["id"]);
  EXPECT_EQ(pairs[0]["later"], response["expiries"][1]["id"]);
  EXPECT_TRUE(pairs[0]["k"].is_number());
  EXPECT_TRUE(pairs[0]["vol_points"].is_number());
  EXPECT_NEAR(pairs[0]["tolerance_vol_points"].get<double>(), .2, 1e-6);
  EXPECT_GT(pairs[0]["vol_points"].get<double>(), .1);
}

TEST(SviApi, InvalidForwardReportsFailedReasonAndNoFittedValues) {
  SviSource source;
  auto m = surface_fixture();
  m.slices[0].forward.forward = kNaN;
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(m);
  const auto response = surface_get(source, "?expiries=1");
  const auto& e = response["expiries"][0];
  EXPECT_TRUE(e["svi"].is_null());
  EXPECT_EQ(e["svi_status"], "failed");
  EXPECT_EQ(e["svi_reason"], "forward must be finite and positive");
  EXPECT_EQ(response["ssvi"]["status"], "too_few_points");
  EXPECT_TRUE(response["ssvi"]["rho"].is_null());
  EXPECT_TRUE(response["ssvi"]["reason"].is_string());
  EXPECT_TRUE(e["ssvi_theta"].is_null());
  EXPECT_EQ(response, surface_get(source, "?expiries=1"));
  EXPECT_TRUE(response["calendar_violations"].empty());
}

TEST(SviApi, CoalescesConcurrentColdSsviRequestsAndCachesFailures) {
  for (bool failure : {false, true}) {
    SviSource source;
    auto m = surface_fixture();
    if (failure) m.slices.resize(1);
    source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(m);
    auto request = [&] { return surface_get(source, "?window=0"); };
    auto a = std::async(std::launch::async, request);
    auto b = std::async(std::launch::async, request);
    const auto response = a.get();
    EXPECT_EQ(response, b.get());
    EXPECT_EQ(response, request());
    EXPECT_EQ(response["ssvi"]["status"], failure ? "too_few_points" : "ok");
  }
}

TEST(SviApi, ReusesFitsAcrossWindowsPrefixesAndConcurrentRequestsInvalidatesNewVersions) {
  SviSource source;
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(surface_fixture());
  auto one = surface_get(source, "?expiries=1&window=0.05");
  auto request = [&] { return surface_get(source, "?expiries=3&window=0.2"); };
  auto concurrent = std::async(std::launch::async, request);
  const auto full = request();
  const auto other = concurrent.get();
  EXPECT_EQ(full, other);  // Includes original nanosecond-resolution measured fit times.
  EXPECT_EQ(one["expiries"][0]["svi"], full["expiries"][0]["svi"]);
  EXPECT_EQ(one["ssvi"], full["ssvi"]);  // One full-underlying fit, even for a prefix.
  EXPECT_EQ(one["expiries"][0]["ssvi_theta"], full["expiries"][0]["ssvi_theta"]);
  EXPECT_LT(one["expiries"][0]["points"].size(), full["expiries"][0]["points"].size());
  EXPECT_TRUE(one["calendar_violations"].empty());
  EXPECT_EQ(full, request());  // Failure timings are also cached.
  auto next = surface_fixture();
  next.version++;
  next.slices[0].strikes.resize(3);
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(next);
  const auto changed = surface_get(source, "?expiries=1");
  EXPECT_EQ(changed["version"], 124);
  EXPECT_EQ(changed["ssvi"]["status"], "too_few_points");
  EXPECT_NE(changed["ssvi"], one["ssvi"]);
  EXPECT_TRUE(changed["expiries"][0]["svi"].is_null());
  // Another source with the same symbol and version must not share the old fit.
  SviSource isolated;
  next.version = 123;
  isolated.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(next);
  EXPECT_TRUE(surface_get(isolated)["expiries"][0]["svi"].is_null());
  EXPECT_EQ(surface_get(isolated)["ssvi"]["status"], "too_few_points");
}
}  // namespace
#endif

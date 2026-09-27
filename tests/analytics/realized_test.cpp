#include "openport/analytics/realized.hpp"
#include "openport/analytics/comparison.hpp"

#include <gtest/gtest.h>
#include <cmath>
#include <set>

namespace {
using namespace openport;
using namespace openport::analytics;
std::vector<md::Bar> history(int count) {
  std::vector<md::Bar> bars;
  auto day = md::days_since_epoch({2026, 1, 2});
  for (int i = 0; i < count; ++day) {
    const auto date = md::date_from_days(day);
    const auto time = md::new_york_to_utc(date, 9, 30);
    if (!md::market_session(time).open) continue;
    const double close = 100 * std::exp(.01 * (i % 3));
    bars.push_back({time, 100, std::max(102.1, close), 99, close});
    ++i;
  }
  return bars;
}

TEST(Realized, HandComputedEstimatorsAndYangZhangOvernightTerm) {
  auto bars = history(3);
  bars[0].close = 100;
  bars[1].open = 100; bars[1].close = 110; bars[1].high = 112; bars[1].low = 98;
  bars[2].open = 108; bars[2].close = 105; bars[2].high = 111; bars[2].low = 103;
  const double r1 = std::log(1.1), r2 = std::log(105.0/110);
  const double oc1 = r1, oc2 = std::log(105.0/108), overnight = std::log(108.0/110);
  const double hl1 = std::log(112.0/98), hl2 = std::log(111.0/103);
  const double park = (hl1*hl1 + hl2*hl2) / (8*std::log(2.0));
  const double gk = .25*(hl1*hl1 + hl2*hl2) - (2*std::log(2.0)-1)*(oc1*oc1 + oc2*oc2)/2;
  const double rs = (std::log(1.12)*std::log(112.0/110) + std::log(.98)*std::log(98.0/110)
      + std::log(111.0/108)*std::log(111.0/105) + std::log(103.0/108)*std::log(103.0/105)) / 2;
  const double k = .34/(1.34+3);
  const double yz = overnight*overnight/2 + k*(oc1-oc2)*(oc1-oc2)/2 + (1-k)*rs;
  const auto result = realized_window(bars, 2);
  EXPECT_NEAR(result.close_to_close.vol, 100*std::sqrt(252*(r1-r2)*(r1-r2)/2), 1e-10);
  EXPECT_NEAR(result.parkinson.vol, 100*std::sqrt(252*park), 1e-10);
  EXPECT_NEAR(result.garman_klass.vol, 100*std::sqrt(252*gk), 1e-10);
  EXPECT_NEAR(result.yang_zhang.vol, 100*std::sqrt(252*yz), 1e-10);
}

TEST(Realized, InvalidOhlcFallsBackButInvalidClosesAndGapsAreMissing) {
  auto bars = history(6);
  bars[2].open = 0;
  auto r = realized_window(bars, 5);
  for (const auto& e : {r.parkinson, r.garman_klass, r.yang_zhang}) {
    EXPECT_TRUE(e.fallback); EXPECT_EQ(e.vol, r.close_to_close.vol);
  }
  bars[2].close = 0;
  EXPECT_EQ(realized_window(bars, 5).close_to_close.reason, "invalid_close");
  bars = history(7); bars.erase(bars.begin()+2);
  EXPECT_EQ(realized_window(bars, 5).close_to_close.reason, "missing_sessions");
  EXPECT_EQ(realized_window(history(5), 5).close_to_close.reason, "insufficient_history");
}

TEST(Realized, ConesUseLinearQuantilesAndTieAwareCurrentPercentile) {
  const std::vector<double> values{4, 1, 3, 2};
  EXPECT_DOUBLE_EQ(percentile(values, .1), 1.3);
  EXPECT_DOUBLE_EQ(percentile(values, .5), 2.5);
  EXPECT_DOUBLE_EQ(percentile(values, 1), 4);
  EXPECT_TRUE(std::isnan(percentile({}, .5)));
  auto bars = history(12);
  const auto cone = vol_cone(bars, 5);
  EXPECT_EQ(cone.observations, 7u);
  EXPECT_EQ(cone.days_used, 12u);
  EXPECT_GE(cone.current_percentile, 0);
  EXPECT_LE(cone.current_percentile, 100);
  for (auto& bar : bars) bar.open = bar.high = bar.low = bar.close = 100;
  const auto flat = vol_cone(bars, 5);
  EXPECT_DOUBLE_EQ(flat.current, 0);
  EXPECT_DOUBLE_EQ(flat.current_percentile, 50);
}

TEST(Realized, IntradayUsesCompleteFiveMinuteBlocksAndSkipsFutureAndZeroRows) {
  const auto open = md::new_york_to_utc({2026, 9, 25}, 9, 30);
  std::vector<md::Bar> minutes;
  for (int i = 0; i < 15; ++i) {
    const double a = 100*std::exp(.001*i), b = 100*std::exp(.001*(i+1));
    minutes.push_back({open+i*md::kNanosPerMinute, a, b, a, b});
  }
  minutes.push_back({open-md::kNanosPerMinute, 0, 0, 0, 0});
  const auto result = intraday_realized(minutes, open+12*md::kNanosPerMinute);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].returns, 2);
  EXPECT_EQ(result[0].observed_minutes, 10);
  EXPECT_TRUE(result[0].partial);
  EXPECT_NEAR(result[0].vol, 100*std::sqrt(252*78*.005*.005), 1e-9);
  minutes.erase(minutes.begin()+3);
  const auto gap = intraday_realized(minutes, open+15*md::kNanosPerMinute);
  EXPECT_EQ(gap[0].returns, 2);
  EXPECT_NEAR(gap[0].vol, result[0].vol, 1e-9);
}

TEST(Realized, DailyMetricsExcludeUnfinishedAndFutureBars) {
  const auto bars = history(8);
  const auto r = realized_metrics(bars, {}, bars[6].start + 60*md::kNanosPerMinute);
  EXPECT_EQ(r.daily_as_of, md::format_date(md::new_york_time(bars[5].start).date));
  EXPECT_EQ(r.cones[0].days_used, 6u);
  EXPECT_EQ(r.cones[0].observations, 1u);
  EXPECT_TRUE(std::isnan(r.today));
}

TEST(VolComparison, StatisticsIgnoreMissingAndInterpolateQuantiles) {
  const std::vector<double> differences{-3, -1, 2, 4, kNaN};
  const auto r = comparison_stats(differences);
  EXPECT_EQ(r.samples, 4u);
  EXPECT_EQ(r.median_absolute, 2.5);
  EXPECT_NEAR(r.p90_absolute, 3.7, 1e-12);
  EXPECT_EQ(r.max_absolute, 4);
  EXPECT_EQ(r.median_signed, .5);
  EXPECT_TRUE(std::isnan(comparison_stats({}).median_signed));
}

TEST(VolComparison, AlignsByClosingMinuteAndLastSampleWinsWithoutNearestFill) {
  const auto open = md::new_york_to_utc({2026, 9, 25}, 9, 30);
  const std::vector<md::Bar> bars{{open, 20, 20, 20, 20}, {open+md::kNanosPerMinute, 21, 21, 21, 21}};
  const std::vector<ComparisonSample> samples{{open, 999}, {open+md::kNanosPerMinute, 22},
      {open+md::kNanosPerMinute+10*md::kNanosPerSecond, 23}, {open+2*md::kNanosPerMinute, 20}};
  const auto stats = compare_minutes(samples, bars);
  EXPECT_EQ(stats.samples, 2u);
  EXPECT_DOUBLE_EQ(stats.median_signed, 1);
  EXPECT_DOUBLE_EQ(stats.median_absolute, 2);
}
}  // namespace

namespace {
TEST(Realized, FullRetainedHistoryProducesEveryRollingWindow) {
  const auto bars = history(2600);
  const auto result = realized_metrics(bars, {}, bars.back().start + 7*60*md::kNanosPerMinute);
  ASSERT_EQ(result.cones.size(), 6u);
  for (const auto& cone : result.cones) {
    EXPECT_EQ(cone.days_used, 2600u);
    EXPECT_EQ(cone.observations, 2600u - static_cast<std::size_t>(cone.sessions));
    EXPECT_TRUE(std::isfinite(cone.p10));
    EXPECT_GE(cone.p90, cone.p10);
  }
}
}  // namespace

namespace {
TEST(Realized, RangeEstimatorsNeedOnlyNBarWindowWhileReturnEstimatorsNeedPriorClose) {
  const auto result = realized_window(history(5), 5);
  EXPECT_TRUE(std::isnan(result.close_to_close.vol));
  EXPECT_TRUE(std::isnan(result.yang_zhang.vol));
  EXPECT_TRUE(std::isfinite(result.parkinson.vol));
  EXPECT_TRUE(std::isfinite(result.garman_klass.vol));
  EXPECT_FALSE(result.parkinson.fallback);
}
}  // namespace

namespace {
TEST(Realized, ConeWindowsMatchDirectEstimatorAcrossInvalidClosesAndCalendarGaps) {
  auto bars = history(40);
  bars[8].close = 0;
  bars.erase(bars.begin()+22);
  std::vector<double> expected;
  for (std::size_t i = 5; i < bars.size(); ++i) {
    const auto window = std::span<const md::Bar>(bars).subspan(i-5, 6);
    const double value = realized_window(window, 5).close_to_close.vol;
    if (std::isfinite(value)) expected.push_back(value);
  }
  const auto cone = vol_cone(bars, 5);
  EXPECT_EQ(cone.observations, expected.size());
  EXPECT_DOUBLE_EQ(cone.p10, percentile(expected, .1));
  EXPECT_DOUBLE_EQ(cone.p50, percentile(expected, .5));
  EXPECT_DOUBLE_EQ(cone.p90, percentile(expected, .9));
  EXPECT_DOUBLE_EQ(cone.current, realized_window(bars, 5).close_to_close.vol);
  bars.back().close = 0;
  EXPECT_TRUE(std::isnan(vol_cone(bars, 5).current));
}
}  // namespace

// Before 2022 the md calendar has no holiday rules, so a trading history that skips
// Christmas and New Year's Day must still count as consecutive sessions, while a gap of
// more than a week is still a missing stretch.
TEST(Realized, HistoryBeforeTheHolidayCalendarSkipsHolidaysButNotLongGaps) {
  std::vector<md::Bar> bars;
  const std::set<md::Date> closed{{2019, 12, 25}, {2020, 1, 1}};
  auto day = md::days_since_epoch({2019, 12, 2});
  for (int i = 0; bars.size() < 30; ++day, ++i) {
    const auto date = md::date_from_days(day);
    const int weekday = static_cast<int>((day % 7 + 11) % 7);  // 1970-01-01 was a Thursday
    if (weekday == 0 || weekday == 6 || closed.contains(date)) continue;
    const double close = 100 * std::exp(.01 * (i % 3));
    bars.push_back({md::new_york_to_utc(date, 9, 30), 100, std::max(102.1, close), 99, close});
  }
  const auto window = realized_window(bars, 21);
  EXPECT_TRUE(window.close_to_close.reason.empty()) << window.close_to_close.reason;
  EXPECT_TRUE(std::isfinite(window.close_to_close.vol));
  EXPECT_GT(vol_cone(bars, 21).observations, 0U);
  auto gapped = bars;
  gapped.erase(gapped.begin() + 10, gapped.begin() + 17);  // seven sessions, ten calendar days
  EXPECT_EQ(realized_window(gapped, 21).close_to_close.reason, "missing_sessions");
}

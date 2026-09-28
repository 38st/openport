#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include "openport/server/equity.hpp"
#include "support/scripted_market.hpp"

namespace openport::server {
namespace {
using trading::Money;
Money m(std::string_view value) { return Money::parse(value); }
struct Directory {
  std::filesystem::path path = std::filesystem::temp_directory_path() / ("openport-equity-" + std::to_string(md::now()));
  Directory() { std::filesystem::create_directories(path); }
  ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
TEST(EquityStore, PersistsMinuteAndFillSamplesReloadsAndLeavesGaps) {
  Directory dir;
  const auto file = dir.path / "account.jsonl.equity.csv";
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  EquitySample sample{time, 1, m("10000"), m("9000"), m("10000"), m("11000"), {}, 0};
  {
    EquityStore store(file);
    store.append(sample);
    sample.time += md::kNanosPerSecond; store.append(sample);
    EXPECT_EQ(store.samples().size(), 1U);
    sample.fill = 1; sample.equity = m("9989.35"); store.append(sample); store.append(sample);
    sample.fill = 2; store.append(sample);
    ASSERT_EQ(store.samples().size(), 3U);
    EXPECT_TRUE(store.error().empty());
  }
  EquityStore recovered(file);
  ASSERT_EQ(recovered.samples().size(), 3U);
  EXPECT_EQ(recovered.samples()[1].equity, m("9989.35"));
  sample.time += 10 * md::kNanosPerMinute; sample.fill = 0; recovered.append(sample);
  ASSERT_EQ(recovered.samples().size(), 4U);
  EXPECT_EQ(recovered.samples().back().time, sample.time);
  EXPECT_FALSE(recovered.samples().back().tomorrow_floor);
}
TEST(EquityStore, CompactsOldAttemptsButKeepsCurrentAndDoesNotThrowOnStorageFailure) {
  Directory dir;
  const auto file = dir.path / "history.csv";
  const auto time = md::new_york_to_utc({2026, 1, 2}, 10, 0);
  EquityStore store(file);
  EquitySample sample{time, 1, m("10000"), {}, m("10000"), {}, {}, 0};
  store.append(sample);
  sample.time += 100 * md::kNanosPerDay; store.append(sample);
  EXPECT_EQ(store.samples().size(), 2U);
  sample.attempt = 2; sample.time += md::kNanosPerMinute; store.append(sample);
  EXPECT_EQ(store.samples().size(), 2U);
  EquityStore failed(dir.path); // A directory cannot be an appendable CSV.
  EXPECT_NO_THROW(failed.append(sample));
  EXPECT_FALSE(failed.error().empty());
  EXPECT_EQ(EquityStore(file).samples().size(), 2U);
}
TEST(EquityStore, TornRowsAreIgnoredWithoutLosingGoodSamples) {
  Directory dir; const auto file = dir.path / "history.csv";
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  { EquityStore store(file); store.append({time, 1, m("10000"), {}, m("10000"), {}, {}, 0}); }
  { std::ofstream out(file, std::ios::app); out << "torn,partial"; }
  EquityStore recovered(file);
  EXPECT_EQ(recovered.samples().size(), 1U);
  EXPECT_FALSE(recovered.error().empty());
}
TEST(EquityStore, EachFillGetsItsExactMarkedEquityAndAtomicLegsShareAMark) {
  test::ScriptedMarket f;
  trading::SessionConfig c;
  c.limits.aggregate = {1e9, 1e9}; c.limits.per_underlying = {1e9, 1e9};
  c.rules.max_drawdown = m("1000");
  trading::TradingSession s(c, f.time); f.seed(s);
  s.submit(f.limit("first", 1, "4"), f.time);
  s.submit(f.limit("second", 1, "4"), f.time);
  const auto before = s.snapshot();
  f.next(); s.on_quotes({f.quote("3.80", "4", 10)}, {f.valuation()}, f.time);
  const auto samples = fill_equity_samples(s, *before);
  ASSERT_EQ(samples.size(), 2U);
  EXPECT_EQ(samples[0].equity, m("99989.35"));
  EXPECT_EQ(samples[1].equity, m("99978.70"));
  EXPECT_EQ(samples[1].equity, s.snapshot()->equity);
  EXPECT_EQ(samples[0].floor, m("99000"));
  const auto with_open = fill_equity_samples(s, *before, true);
  ASSERT_EQ(with_open.size(), 3U);
  EXPECT_EQ(with_open[0].equity, m("100000"));
  EXPECT_EQ(with_open[0].fill, 0U);
  EXPECT_EQ(with_open[1].equity, samples[0].equity);
  EXPECT_EQ(with_open[2].equity, samples[1].equity);
  test::ScriptedMarket wing; wing.contract.strike = 5010; wing.time = f.time; wing.seed(s, "2", "2.20");
  auto combo = f.market("combo"); combo.symbol.clear(); combo.legs = {{f.symbol(), trading::Side::Buy, 1}, {wing.symbol(), trading::Side::Sell, 1}};
  const auto prior = s.snapshot(); s.submit(combo, f.time);
  const auto legs = fill_equity_samples(s, *prior);
  ASSERT_EQ(legs.size(), 2U);
  EXPECT_EQ(legs[0].equity, legs[1].equity);
  EXPECT_EQ(legs[1].equity, s.snapshot()->equity);
}
TEST(EquityStore, PreFillObservationPreservesAHighBeforeTheSameQuoteClosesThePosition) {
  test::ScriptedMarket market;
  trading::SessionConfig config;
  config.limits.aggregate = {1e9, 1e9}; config.limits.per_underlying = {1e9, 1e9};
  config.limits.price_band_absolute = m("10");
  trading::TradingSession session(config, market.time);
  market.seed(session);
  ASSERT_TRUE(session.submit(market.market("entry"), market.time).decision.ok());
  auto exit = market.limit("exit", 1, "7"); exit.side = trading::Side::Sell;
  ASSERT_TRUE(session.submit(exit, market.time).decision.ok());
  const auto before = session.snapshot();
  market.next(); session.on_quotes({market.quote("7", "7.20", 10)}, {market.valuation()}, market.time);
  const auto samples = fill_equity_samples(session, *before, true);
  ASSERT_EQ(samples.size(), 2U);
  EXPECT_EQ(samples[0].equity, m("100289.35"));
  EXPECT_EQ(samples[1].equity, m("100278.70"));
  EXPECT_EQ(samples[0].fill, 0U);
  EXPECT_NE(samples[1].fill, 0U);
}
TEST(EquityStore, ShareChangesAreSampledAndEightColumnHistoryStillLoads) {
  Directory dir; const auto file = dir.path / "history.csv";
  test::ScriptedMarket f; f.contract = *md::parse_osi("SPY261022C00500000");
  trading::SessionConfig c; c.limits.aggregate = {1e9, 1e9}; c.limits.per_underlying = {1e9, 1e9};
  trading::TradingSession s(c, f.time);
  auto valuation = f.valuation(); valuation.spot = 505; valuation.forward = 505;
  s.define(f.contract, f.time);
  s.on_quotes({f.quote("6", "6.20")}, {valuation}, f.time, {{"SPY", f.time, m("505")}});
  ASSERT_TRUE(s.submit(f.market("entry"), f.time).decision.ok());
  auto before = s.snapshot();
  ASSERT_TRUE(s.exercise(f.symbol(), 1, f.time).decision.ok());
  auto samples = fill_equity_samples(s, *before);
  ASSERT_EQ(samples.size(), 1U);
  EXPECT_EQ(samples.front().stock_fill, 1U);
  EXPECT_EQ(samples.front().equity, s.snapshot()->equity);
  { std::ofstream out(file); out << f.time << ",1,100000000000,null,100000000000,null,null,0\n"; }
  EquityStore store(file);
  store.append(samples.front()); store.append(samples.front());
  EXPECT_EQ(store.samples().size(), 2U);
  before = s.snapshot();
  ASSERT_TRUE(s.trade_stock("SPY", -100, f.time).decision.ok());
  samples = fill_equity_samples(s, *before);
  ASSERT_EQ(samples.size(), 1U);
  store.append(samples.front());
  const EquityStore recovered(file);
  ASSERT_EQ(recovered.samples().size(), 3U);
  EXPECT_EQ(recovered.samples().back().stock_fill, 2U);
  EXPECT_TRUE(recovered.error().empty());
}
}
}

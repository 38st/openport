#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig roomy() {
  SessionConfig c;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  return c;
}
/// A new quote with the underlying at `spot` and the strike's volatility at `iv`.
void tick(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask, double spot, double iv,
          Timestamp step = md::kNanosPerSecond) {
  f.time += step;
  ++f.observation;
  auto valuation = f.valuation();
  valuation.spot = spot;
  valuation.smile_iv = iv;
  s.on_quotes({f.quote(bid, ask)}, {valuation}, f.time);
}
double day_pnl(const TradingSession& s) { return (s.snapshot()->equity - s.snapshot()->start_of_day_equity).dollars(); }

TEST(TradingAttribution, SplitsAHeldPositionsMoveByItsStartingGreeks) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);  // bid 4.00, ask 4.20: mark 4.10; delta 0.5, gamma 0.001, vega 2, theta -0.1, spot 5000, iv 20%
  ASSERT_TRUE(s.submit(f.market("buy", 2), f.time).decision.ok());
  auto a = s.snapshot()->attribution;
  // Two contracts bought at the 4.20 ask against a 4.10 mark, and 1.30 of fees.
  EXPECT_NEAR(a.costs, -2 * 100 * 0.10 - 1.30, 1e-9);
  EXPECT_NEAR(a.delta + a.gamma + a.vega + a.theta + a.other, 0, 1e-9);
  // Spot +10, volatility +1 point, one day later; the mark rises to 9.10.
  const double years = f.valuation().years;
  tick(s, f, "9.00", "9.20", 5010, 0.21, md::kNanosPerDay);
  a = s.snapshot()->attribution;
  const double size = 2 * 100;
  EXPECT_NEAR(a.delta, size * 0.5 * 10, 1e-6);
  EXPECT_NEAR(a.gamma, size * 0.5 * 0.001 * 100, 1e-6);
  EXPECT_NEAR(a.vega, size * 2.0 * 1, 1e-6);
  EXPECT_NEAR(a.theta, size * -0.1 * (years - f.valuation().years) * 365, 1e-6);
  EXPECT_NEAR(a.theta, size * -0.1, 1e-3);
  EXPECT_NEAR(a.delta + a.gamma + a.vega + a.theta + a.other, size * (9.10 - 4.10), 1e-6);
  EXPECT_NEAR(a.total(), day_pnl(s), 1e-6);
  const auto& position = s.snapshot()->attributions.at(f.symbol());
  EXPECT_NEAR(position.total(), a.total(), 1e-9);
}

TEST(TradingAttribution, AccountsForEveryDollarOfTheDayAcrossFillsAndSettlement) {
  ScriptedMarket f;
  f.contract.expiry = {2026, 9, 22};  // expires today at 16:00
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 3), f.time).decision.ok());
  tick(s, f, "4.40", "4.60", 5004, 0.195);
  ASSERT_TRUE(s.submit(f.market("trim", 1, Side::Sell), f.time).decision.ok());
  tick(s, f, "4.60", "4.80", 5006, 0.19);
  ASSERT_TRUE(s.submit(f.market("add", 2), f.time).decision.ok());
  tick(s, f, "3.90", "4.10", 4998, 0.205);
  EXPECT_NEAR(s.snapshot()->attribution.total(), day_pnl(s), 1e-6);
  // Settled at expiry: the last stretch ends at intrinsic value.
  f.time = f.contract.expiry_time();
  s.on_quotes({}, {}, f.time);
  ASSERT_TRUE(s.settle(f.symbol(), m("5003.50"), f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
  const auto a = s.snapshot()->attribution;
  EXPECT_NEAR(a.total(), day_pnl(s), 1e-6);
  EXPECT_LT(a.costs, 0);
  EXPECT_NE(a.delta, 0);
}

TEST(TradingAttribution, EachDayKeepsItsOwnAndTheNextStartsFromTheClose) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-attribution-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = (directory / "journal.jsonl").string();
  ScriptedMarket f;
  std::string expected;
  {
    TradingSession s(roomy(), f.time, FileJournal::create(path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open", 2), f.time).decision.ok());
    tick(s, f, "4.50", "4.70", 5008, 0.2);
    f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
    ++f.observation;
    s.on_quotes({f.quote("4.50", "4.70")}, {f.valuation()}, f.time);
    ASSERT_TRUE(s.roll_day(f.time).decision.ok());
    const auto& day = s.snapshot()->evaluation.days.back();
    EXPECT_NEAR(day.attribution.total(), (day.close_equity - day.open_equity).dollars(), 1e-6);
    EXPECT_NEAR(s.snapshot()->attribution.total(), 0, 1e-9);
    tick(s, f, "4.80", "5.00", 5012, 0.2);
    EXPECT_NEAR(s.snapshot()->attribution.total(), day_pnl(s), 1e-6);
    expected = s.snapshot_json();
  }
  // It is state, so it recovers with the account.
  const auto recovered = TradingSession::recover(FileJournal::read(path));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  std::filesystem::remove_all(directory);
}

TEST(TradingAttribution, WithoutValuationsItIsAllOther) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  // Pre-trade checks need Greeks, so trade with them, then lose them.
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  f.next();
  auto invalid = f.valuation();
  invalid.valid = false;
  s.on_quotes({f.quote("4.50", "4.70")}, {invalid}, f.time);
  const auto a = s.snapshot()->attribution;
  EXPECT_EQ(a.delta, 0);
  EXPECT_EQ(a.vega, 0);
  EXPECT_NEAR(a.other, 100 * (4.60 - 4.10), 1e-6);
}

}  // namespace
}  // namespace openport::trading

#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>

#include "openport/trading/dividends.hpp"
#include "openport/trading/history.hpp"
#include "openport/trading/session.hpp"

namespace openport::trading {
namespace {
Money m(std::string_view s) { return Money::parse(s); }
std::vector<Dividend> parse(const std::string& text) {
  std::istringstream in(text);
  return parse_dividends(in);
}

TEST(TradingDividends, ReadsAFileOfExDatesAndAmounts) {
  const auto all = parse("symbol,ex_date,amount\n# quarterly\n\nQQQ,2026-12-21,0.70\n SPY , 2026-12-18 , 1.90 \n");
  ASSERT_EQ(all.size(), 2U);
  EXPECT_EQ(all[0].symbol, "SPY");  // sorted by ex-date
  EXPECT_EQ(all[0].ex_date, (md::Date{2026, 12, 18}));
  EXPECT_EQ(all[0].per_share, m("1.90"));
  EXPECT_EQ(all[1].symbol, "QQQ");
  for (const auto* bad : {"SPY,2026-12-18", "SPY,2026-13-01,1.90", "spy,2026-12-18,1.90", "SPY,2026-12-18,-1",
                          "SPY,2026-12-18,abc", "SPY,2026-12-18,1.90,x", "SPY,2026-12-18,1.90\nSPY,2026-12-18,2"}) {
    EXPECT_THROW(parse(bad), std::invalid_argument) << bad;
  }
  try {
    parse("SPY,2026-12-18,1.90\nQQQ,soon,1\n");
    FAIL();
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("line 2"), std::string::npos);
  }
  // A rollover pays what went ex after the last trading date, through the new one.
  const auto due = dividends_due(all, {2026, 12, 17}, {2026, 12, 21});
  EXPECT_EQ(due.size(), 2U);
  EXPECT_TRUE(dividends_due(all, {2026, 12, 18}, {2026, 12, 18}).empty());
}

/// SPY shares through an exercise, then SPY's own price in every batch.
struct Held {
  md::OptionContract call = *md::parse_osi("SPY261218C00500000");
  md::Timestamp time = md::new_york_to_utc({2026, 12, 17}, 10, 0);
  std::uint64_t observation = 0;
  void quote(TradingSession& s, double spot) {
    ++observation;
    s.on_quotes({{call.osi_symbol(), observation, time, m("20.00"), m("20.20"), 10, 10}},
                {{call.osi_symbol(), time, 0.9, 0.01, 0.2, -0.05, spot, spot, 0.99, md::years_between(time, call.expiry_time()), 0.2, true}},
                time, {{"SPY", time, Money::from_double(spot)}});
  }
};
SessionConfig roomy() {
  SessionConfig c;
  c.limits.aggregate = {1e12, 1e12};
  c.limits.per_underlying = {1e12, 1e12};
  return c;
}

TEST(TradingDividends, SharesHeldIntoTheExDateReceiveItAndItJoinsTheirRoundTrip) {
  Held f;
  TradingSession s(roomy(), f.time);
  ASSERT_TRUE(s.define(f.call, f.time).decision.ok());
  f.quote(s, 520);
  ASSERT_TRUE(s.submit({"calls", f.call.osi_symbol(), Side::Buy, OrderType::Market, TimeInForce::Ioc, 2, {}, {}, {}, {}}, f.time).decision.ok());
  ASSERT_TRUE(s.exercise(f.call.osi_symbol(), 2, f.time).decision.ok());  // 200 SPY at 520
  const auto cash = s.snapshot()->account.cash;
  f.time = md::new_york_to_utc({2026, 12, 17}, 16, 0);
  f.quote(s, 521);
  const auto night = md::new_york_to_utc({2026, 12, 17}, 18, 0);
  s.on_quotes({}, {}, night);
  const std::vector<Dividend> due{{"SPY", {2026, 12, 18}, m("1.90")}, {"QQQ", {2026, 12, 18}, m("0.70")}};
  ASSERT_TRUE(s.roll_day(night, due).decision.ok());
  auto snap = s.snapshot();
  EXPECT_EQ(snap->account.cash, cash + m("380"));  // 200 shares at 1.90; no QQQ held
  ASSERT_EQ(snap->dividends.size(), 1U);
  EXPECT_EQ(snap->dividends[0].shares, 200);
  EXPECT_EQ(snap->dividends[0].amount, m("380"));
  EXPECT_NEAR(snap->attribution.other, 380, 1e-9);
  // Once per ex-date.
  f.time = md::new_york_to_utc({2026, 12, 18}, 10, 0);
  f.quote(s, 519.10);
  ASSERT_TRUE(s.trade_stock("SPY", -200, f.time).decision.ok());
  snap = s.snapshot();
  const auto trips = share_lifecycles(snap->stock_fills, snap->dividends);
  ASSERT_EQ(trips.size(), 1U);
  EXPECT_EQ(trips[0].dividends, m("380"));
  EXPECT_EQ(trips[0].gross, m("-180"));  // bought at 520, sold at 519.10
}

TEST(TradingDividends, ShortSharesPayIt) {
  Held f;
  TradingSession s(roomy(), f.time);
  ASSERT_TRUE(s.define(f.call, f.time).decision.ok());
  f.quote(s, 520);
  ASSERT_TRUE(s.submit({"sold", f.call.osi_symbol(), Side::Sell, OrderType::Market, TimeInForce::Ioc, 10, {}, {}, {}, {}}, f.time).decision.ok());
  // Partly assigned overnight as the market prices the call under its exercise value, then short into the ex-date.
  f.time = md::new_york_to_utc({2026, 12, 17}, 16, 14, 30);
  ++f.observation;
  s.on_quotes({{f.call.osi_symbol(), f.observation, f.time, m("19.80"), m("19.90"), 10, 10}},
              {{f.call.osi_symbol(), f.time, 0.9, 0.01, 0.2, -0.05, 520, 520, 0.99, md::years_between(f.time, f.call.expiry_time()), 0.2, true}},
              f.time, {{"SPY", f.time, m("520")}});
  const auto night = md::new_york_to_utc({2026, 12, 17}, 18, 0);
  s.on_quotes({}, {}, night);
  const auto cash = s.snapshot()->account.cash;
  ASSERT_TRUE(s.roll_day(night, {{"SPY", {2026, 12, 18}, m("1.90")}}).decision.ok());
  const auto snap = s.snapshot();
  ASSERT_EQ(snap->closures.size(), 1U);
  const Quantity assigned = -snap->closures[0].quantity;
  ASSERT_GT(assigned, 0);
  ASSERT_EQ(snap->dividends.size(), 1U);
  EXPECT_EQ(snap->dividends[0].shares, -100 * assigned);
  EXPECT_EQ(snap->dividends[0].amount, m("-190") * assigned);
  // The buy-back at intrinsic and the short sale at 520 net to the strike; then the dividend is paid.
  EXPECT_EQ(snap->account.cash, cash + (m("-20") * 100 + m("520") * 100 - m("190")) * assigned);
}

TEST(TradingDividends, CallsWithLessTimeValueThanTheDividendAreAssignedBeforeIt) {
  const auto roll = [](std::vector<Dividend> due) {
    Held f;
    auto s = std::make_unique<TradingSession>(roomy(), f.time);
    EXPECT_TRUE(s->define(f.call, f.time).decision.ok());
    f.quote(*s, 520);
    EXPECT_TRUE(s->submit({"sold", f.call.osi_symbol(), Side::Sell, OrderType::Market, TimeInForce::Ioc, 10, {}, {}, {}, {}}, f.time).decision.ok());
    // At 20.10 against 20 of exercise value, the call keeps 0.10 of time value.
    f.time = md::new_york_to_utc({2026, 12, 17}, 16, 14, 30);
    f.quote(*s, 520);
    const auto night = md::new_york_to_utc({2026, 12, 17}, 18, 0);
    s->on_quotes({}, {}, night);
    EXPECT_TRUE(s->roll_day(night, due).decision.ok());
    return s;
  };
  // Without a dividend a holder keeps it.
  EXPECT_TRUE(roll({})->snapshot()->closures.empty());
  // Exercising takes the 1.90 dividend for 0.10 of time value, so holders do.
  const auto s = roll({{"SPY", {2026, 12, 18}, m("1.90")}});
  const auto snap = s->snapshot();
  ASSERT_EQ(snap->closures.size(), 1U);
  const Quantity assigned = -snap->closures[0].quantity;
  EXPECT_GT(assigned, 0);
  EXPECT_LT(assigned, 10);
  ASSERT_EQ(snap->dividends.size(), 1U);
  EXPECT_EQ(snap->dividends[0].shares, -100 * assigned);
}

}  // namespace
}  // namespace openport::trading

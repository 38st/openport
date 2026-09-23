#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

#include "openport/trading/history.hpp"
#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig roomy(AccountRules rules = {}, std::string_view cash = "100000") {
  SessionConfig c;
  c.initial_cash = m(cash);
  c.limits.aggregate = {1e12, 1e12};
  c.limits.per_underlying = {1e12, 1e12};
  c.limits.max_daily_loss = m("1000000");
  c.rules = std::move(rules);
  return c;
}
/// SPY options, with SPY's own price in every batch.
struct Spy {
  md::Timestamp time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  std::uint64_t observation = 0;
  double spot = 510;
  void define(TradingSession& s, const md::OptionContract& c) { ASSERT_TRUE(s.define(c, time).decision.ok()); }
  void quote(TradingSession& s, const md::OptionContract& c, std::string_view bid, std::string_view ask) {
    ++observation;
    s.on_quotes({{c.osi_symbol(), observation, time, m(bid), m(ask), 10, 10}},
                {{c.osi_symbol(), time, c.type == pricing::OptionType::Call ? 0.6 : -0.4, 0.02, 0.5, -0.05, spot, spot, 0.99,
                  md::years_between(time, c.expiry_time()), 0.2, true}},
                time, {{"SPY", time, Money::from_double(spot)}});
  }
  void price(TradingSession& s) { s.on_quotes({}, {}, time, {{"SPY", time, Money::from_double(spot)}}); }
  OrderRequest market(std::string client, const md::OptionContract& c, Quantity quantity, Side side = Side::Buy) const {
    return {std::move(client), c.osi_symbol(), side, OrderType::Market, TimeInForce::Ioc, quantity, {}, {}, {}, {}};
  }
};
const MarkedStock* stock(const TradingSession& s, std::string_view symbol) {
  for (const auto& p : s.snapshot()->stocks) if (p.position.symbol == symbol) return &p;
  return nullptr;
}
double day_pnl(const TradingSession& s) { return (s.snapshot()->equity - s.snapshot()->start_of_day_equity).dollars(); }

TEST(TradingDelivery, ExpiryExercisesAndAssignsEquityOptionsACentInTheMoney) {
  const auto call = *md::parse_osi("SPY260922C00500000");  // expires today at 16:00
  const auto put = *md::parse_osi("SPY260922P00500000");
  Spy f;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.define(s, put);
  f.quote(s, call, "10.00", "10.20");
  f.quote(s, put, "0.10", "0.20");
  ASSERT_TRUE(s.submit(f.market("calls", call, 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("puts", put, 1), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->account.cash, m("97938.05"));  // 2040 + 20 of premium, 1.95 of fees
  f.time = call.expiry_time();
  s.on_quotes({}, {}, f.time);
  // The calls are exercised: settled at 10.00, they buy 200 shares at 510.00.
  ASSERT_TRUE(s.settle(call.osi_symbol(), m("510"), f.time).decision.ok());
  const auto* shares = stock(s, "SPY");
  ASSERT_NE(shares, nullptr);
  EXPECT_EQ(shares->position.shares, 200);
  EXPECT_EQ(shares->position.basis, m("102000"));
  EXPECT_EQ(s.snapshot()->account.cash, m("97938.05") + m("2000") - m("102000"));
  // Together they paid the strike: 500.00 a share.
  EXPECT_EQ(m("2000") - m("102000"), m("-100000"));
  // The put, out of the money, expires worthless.
  ASSERT_TRUE(s.settle(put.osi_symbol(), m("510"), f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, 200);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_NEAR(s.snapshot()->attribution.total(), day_pnl(s), 1e-6);
  // The option trades close at their settlement; the shares stay out of the trade history.
  const auto trades = lifecycles(s.snapshot()->recent_fills, s.snapshot()->closures, s.contracts());
  ASSERT_EQ(trades.size(), 2U);
  for (const auto& t : trades) EXPECT_EQ(t.closure, ClosureKind::Settlement);
}

TEST(TradingDelivery, ShortsAreAssignedAndShortSharesHoldBuyingPower) {
  const auto call = *md::parse_osi("SPY260922C00500000");
  const auto put = *md::parse_osi("SPY260922P00500000");
  AccountRules rules;
  rules.buying_power = true;
  Spy f;
  TradingSession s(roomy(rules), f.time);
  f.define(s, call);
  f.define(s, put);
  f.quote(s, call, "10.00", "10.20");
  f.quote(s, put, "0.10", "0.20");
  ASSERT_TRUE(s.submit(f.market("short-call", call, 1, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("short-put", put, 2, Side::Sell), f.time).decision.ok());
  f.time = call.expiry_time();
  s.on_quotes({}, {}, f.time);
  // Short a call at 510: assigned, it sells 100 shares at 510.
  ASSERT_TRUE(s.settle(call.osi_symbol(), m("510"), f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, -100);
  // Half a cent in the money is not exercised.
  ASSERT_TRUE(s.settle(put.osi_symbol(), m("499.995"), f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, -100);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, m("76500"));  // 150% of 51,000
}

TEST(TradingDelivery, ShortPutsAssignedBuySharesAndIndexOptionsStayCash) {
  const auto put = *md::parse_osi("SPY260922P00500000");
  Spy f;
  TradingSession s(roomy(), f.time);
  f.define(s, put);
  f.quote(s, put, "0.10", "0.20");
  ASSERT_TRUE(s.submit(f.market("short-put", put, 3, Side::Sell), f.time).decision.ok());
  test::ScriptedMarket index;  // SPXW, European and cash-settled
  index.contract.expiry = {2026, 9, 22};
  index.seed(s);
  ASSERT_TRUE(s.submit(index.market("spx"), index.time).decision.ok());
  f.time = put.expiry_time();
  s.on_quotes({}, {}, f.time);
  ASSERT_TRUE(s.settle(put.osi_symbol(), m("490"), f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, 300);
  EXPECT_EQ(stock(s, "SPY")->position.basis, m("147000"));
  ASSERT_TRUE(s.settle(index.symbol(), m("5100"), f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPX"), nullptr);
  EXPECT_EQ(s.snapshot()->stocks.size(), 1U);
}

TEST(TradingDelivery, EarlyExerciseGivesUpTimeValueAndKeepsTheRestOpen) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  Spy f;
  f.spot = 520;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.quote(s, call, "21.00", "21.20");
  ASSERT_TRUE(s.submit(f.market("calls", call, 3), f.time).decision.ok());
  const auto cash = s.snapshot()->account.cash;
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 2, f.time).decision.ok());
  // Two contracts close at 20.00 intrinsic and buy 200 shares at 520.00.
  EXPECT_EQ(s.snapshot()->account.cash, cash + m("4000") - m("104000"));
  EXPECT_EQ(s.snapshot()->positions.at(0).position.quantity, 1);
  EXPECT_EQ(stock(s, "SPY")->position.shares, 200);
  // Exercising gave up 1.10 of time value a share against the 21.10 mark.
  const auto a = s.snapshot()->attribution;
  EXPECT_NEAR(a.costs, 3 * 100 * -0.10 - 1.95 + 2 * 100 * (20.00 - 21.10), 1e-6);
  EXPECT_NEAR(a.total(), day_pnl(s), 1e-6);
  const auto trades = lifecycles(s.snapshot()->recent_fills, s.snapshot()->closures, s.contracts());
  ASSERT_EQ(trades.size(), 1U);
  EXPECT_FALSE(trades[0].closed);
  EXPECT_EQ(trades[0].quantity, 1);
  EXPECT_EQ(trades[0].closed_contracts, 2);
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
  const auto closed = lifecycles(s.snapshot()->recent_fills, s.snapshot()->closures, s.contracts());
  EXPECT_TRUE(closed[0].closed);
  EXPECT_EQ(closed[0].closure, ClosureKind::Exercise);
  EXPECT_EQ(stock(s, "SPY")->position.shares, 300);
}

TEST(TradingDelivery, ExerciseNeedsALongInTheMoneyEquityOptionAndItsPrice) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  const auto put = *md::parse_osi("SPY261022P00500000");
  Spy f;
  f.spot = 520;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.define(s, put);
  f.quote(s, call, "21.00", "21.20");
  f.quote(s, put, "1.00", "1.20");
  ASSERT_TRUE(s.submit(f.market("put", put, 1), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("short-call", call, 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.exercise(put.osi_symbol(), 1, f.time).decision.code, Reason::INVALID_ORDER);   // out of the money
  EXPECT_EQ(s.exercise(put.osi_symbol(), 2, f.time).decision.code, Reason::INVALID_ORDER);   // more than held
  EXPECT_EQ(s.exercise(call.osi_symbol(), 1, f.time).decision.code, Reason::INVALID_ORDER);  // short
  EXPECT_THROW((void)s.exercise(put.osi_symbol(), 0, f.time), TradingError);
  test::ScriptedMarket index;
  index.seed(s);
  ASSERT_TRUE(s.submit(index.market("spx"), index.time).decision.ok());
  EXPECT_EQ(s.exercise(index.symbol(), 1, f.time).decision.code, Reason::INVALID_ORDER);    // cash-settled
  // Without a fresh price for SPY there is nothing to exercise against.
  f.time += 5 * md::kNanosPerMinute;
  f.spot = 480;
  s.on_quotes({}, {}, f.time);
  EXPECT_EQ(s.exercise(put.osi_symbol(), 1, f.time).decision.code, Reason::STALE_QUOTE);
  f.price(s);
  EXPECT_TRUE(s.exercise(put.osi_symbol(), 1, f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, -100);
}

TEST(TradingDelivery, ExerciseWithinBuyingPower) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  AccountRules rules;
  rules.buying_power = true;
  Spy f;
  f.spot = 520;
  TradingSession s(roomy(rules, "10000"), f.time);
  f.define(s, call);
  f.quote(s, call, "21.00", "21.20");
  ASSERT_TRUE(s.submit(f.market("call", call, 1), f.time).decision.ok());
  EXPECT_EQ(s.exercise(call.osi_symbol(), 1, f.time).decision.code, Reason::BUYING_POWER);
  EXPECT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_TRUE(s.snapshot()->stocks.empty());
}

TEST(TradingDelivery, SharesAreMarkedRiskedAndClosedAtTheUnderlyingsPrice) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-delivery-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = (directory / "journal.jsonl").string();
  const auto call = *md::parse_osi("SPY261022C00500000");
  Spy f;
  f.spot = 520;
  std::string expected;
  {
    TradingSession s(roomy(), f.time, FileJournal::create(path));
    f.define(s, call);
    f.quote(s, call, "21.00", "21.20");
    ASSERT_TRUE(s.submit(f.market("calls", call, 3), f.time).decision.ok());
    ASSERT_TRUE(s.exercise(call.osi_symbol(), 3, f.time).decision.ok());
    f.time += md::kNanosPerSecond;
    f.spot = 525;
    f.price(s);
    const auto* shares = stock(s, "SPY");
    ASSERT_NE(shares, nullptr);
    EXPECT_TRUE(shares->fresh);
    EXPECT_EQ(shares->mark, m("525"));
    EXPECT_EQ(shares->market_value, m("157500"));
    EXPECT_EQ(shares->unrealised, m("1500"));
    EXPECT_TRUE(s.snapshot()->valuation_complete);
    EXPECT_NEAR(s.snapshot()->attributions.at("SPY").delta, 300 * 5.0, 1e-9);
    EXPECT_DOUBLE_EQ(s.snapshot()->risk.underlyings.at("SPY").position.dollar_delta, 300 * 525.0);
    for (const auto& cell : s.snapshot()->scenarios.cells) {
      if (cell.spot_percent == 1) { EXPECT_NEAR(cell.pnl, 300 * 525.0 * 0.01, 1e-6); }
    }
    // Stock trades only reduce the delivered shares.
    EXPECT_EQ(s.trade_stock("SPY", 100, f.time).decision.code, Reason::INVALID_ORDER);
    EXPECT_EQ(s.trade_stock("SPY", -400, f.time).decision.code, Reason::INVALID_ORDER);
    EXPECT_EQ(s.trade_stock("QQQ", -1, f.time).decision.code, Reason::INVALID_ORDER);
    ASSERT_TRUE(s.trade_stock("SPY", -100, f.time).decision.ok());
    EXPECT_EQ(stock(s, "SPY")->position.shares, 200);
    EXPECT_EQ(stock(s, "SPY")->position.realised, m("500"));
    // Flattening closes the rest at the price.
    ASSERT_TRUE(s.close_positions(std::nullopt, f.time).decision.ok());
    EXPECT_TRUE(s.snapshot()->stocks.empty());
    EXPECT_NEAR(s.snapshot()->attribution.total(), day_pnl(s), 1e-6);
    expected = s.snapshot_json();
  }
  EXPECT_EQ(TradingSession::recover(FileJournal::read(path)).snapshot_json(), expected);
  std::filesystem::remove_all(directory);
}

TEST(TradingDelivery, SharesTradeInTheRegularSessionAndKeepTheirClose) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  Spy f;
  f.spot = 520;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.quote(s, call, "21.00", "21.20");
  ASSERT_TRUE(s.submit(f.market("calls", call, 1), f.time).decision.ok());
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
  f.time = md::new_york_to_utc({2026, 9, 22}, 16, 14);
  f.price(s);
  f.time = md::new_york_to_utc({2026, 9, 22}, 18, 0);
  s.on_quotes({}, {}, f.time);
  // The close stays current overnight, but shares trade in the regular session.
  EXPECT_TRUE(stock(s, "SPY")->fresh);
  EXPECT_EQ(s.trade_stock("SPY", -100, f.time).decision.code, Reason::SESSION_CLOSED);
}

}  // namespace
}  // namespace openport::trading

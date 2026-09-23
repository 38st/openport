#include <gtest/gtest.h>

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
TEST(TradingMatching, MarketFarSidePartialIocAndSharedBudgets) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s, "4", "4.20", 2);
  s.submit(f.market("buy", 3), f.time);
  auto snap = s.snapshot();
  ASSERT_EQ(snap->recent_fills.size(), 1);
  EXPECT_EQ(snap->recent_fills[0].price, m("4.20"));
  EXPECT_EQ(snap->recent_fills[0].quantity, 2);
  EXPECT_EQ(snap->recent_fills[0].fee, m("1.30"));
  EXPECT_EQ(snap->recent_orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(snap->recent_orders[0].filled_quantity, 2);
  EXPECT_EQ(snap->recent_orders[0].reason.code, Reason::IOC_REMAINDER);
  s.submit(f.market("buy2"), f.time);
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 1);
  s.submit(f.market("sell", 1, Side::Sell), f.time);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("4"));
  EXPECT_EQ(s.snapshot()->account.fees, m("1.95"));
  // Old immutable publications remain valid and unchanged.
  EXPECT_EQ(snap->recent_fills.size(), 1);
  EXPECT_EQ(s.snapshot()->equity, m("99968.05"));
}
TEST(TradingMatching, MarketableLimitUsesFarSideAndDayRemainderWaitsForNewObservation) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s, "4", "4.20", 1);
  s.submit(f.limit("day", 3, "4.50"), f.time);
  EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(s.snapshot()->recent_fills[0].price, m("4.20"));
  s.on_quotes({f.quote("4", "4.20", 99)}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 1);
  f.next();
  s.on_quotes({f.quote("4", "4.20", 1)}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 2);
  EXPECT_EQ(s.snapshot()->recent_orders[0].filled_quantity, 2);
  f.next();
  s.on_quotes({f.quote("4", "4.20", 10)}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->account.fees, m("1.95"));
}
TEST(TradingMatching, BuyPriceThenAcceptancePriority) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s, "4", "4.60", 2);
  s.submit(f.limit("first", 2, "4.20"), f.time);
  s.submit(f.limit("better", 1, "4.40"), f.time);
  s.submit(f.limit("third", 1, "4.20"), f.time);
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  f.next();
  s.on_quotes({f.quote("4", "4.20", 2)}, {f.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->recent_fills.size(), 2);
  EXPECT_EQ(s.snapshot()->recent_fills[0].order_id, 2);
  EXPECT_EQ(s.snapshot()->recent_fills[1].order_id, 1);
  EXPECT_EQ(s.snapshot()->recent_orders[2].filled_quantity, 0);
  f.next();
  s.on_quotes({f.quote("4", "4.20", 1)}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->recent_fills.back().order_id, 1);
}
TEST(TradingMatching, SellPriorityAndIocLimitDoesNotRest) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s, "4", "4.60", 1);
  s.submit(f.limit("ioc", 1, "4.20", Side::Sell, TimeInForce::Ioc), f.time);
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::IOC_REMAINDER);
  s.submit(f.limit("expensive", 1, "4.50", Side::Sell), f.time);
  s.submit(f.limit("better", 1, "4.40", Side::Sell), f.time);
  f.next();
  s.on_quotes({f.quote("4.50", "4.60", 1)}, {f.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->recent_fills.size(), 1);
  EXPECT_EQ(s.snapshot()->recent_fills[0].order_id, 3);
  EXPECT_EQ(s.snapshot()->recent_fills[0].price, m("4.50"));
}
TEST(TradingMatching, InvalidBooksNoLiquidityAndLastMarkRetained) {
  for (int mode = 0; mode < 5; ++mode) {
    ScriptedMarket f;
    TradingSession s(roomy(), f.time);
    f.seed(s);
    s.submit(f.market("position"), f.time);
    s.submit(f.limit("rest", 1, "4.10"), f.time);
    f.next();
    auto quote = f.quote("4", "4.10");
    if (mode == 0) quote.bid.reset();
    if (mode == 1) quote.ask.reset();
    if (mode == 2) quote.bid = m("4.20");
    if (mode == 3) quote.ask_size = 0;
    if (mode == 4) quote.bid_size = 0;
    s.on_quotes({quote}, {f.valuation()}, f.time);
    EXPECT_EQ(s.snapshot()->recent_fills.size(), 1) << mode;
    EXPECT_EQ(s.submit(f.market("invalid"), f.time).decision.code, Reason::INVALID_QUOTE);
    ASSERT_EQ(s.snapshot()->positions.size(), 1);
    EXPECT_EQ(s.snapshot()->positions[0].mark, m("4.10"));
    EXPECT_EQ(s.snapshot()->positions[0].mark_age, md::kNanosPerSecond);
    EXPECT_FALSE(s.snapshot()->valuation_complete);
  }
}
TEST(TradingMatching, NoncrossedLimitsDoNotFillAndLimitsBoundExecution) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s, "4", "4.60");
  s.submit(f.limit("buy", 1, "4.20"), f.time);
  s.submit(f.limit("sell", 1, "4.50", Side::Sell), f.time);
  f.next();
  s.on_quotes({f.quote("4.10", "4.30")}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  f.next();
  s.on_quotes({f.quote("4", "4.20")}, {f.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->recent_fills.size(), 1);
  EXPECT_LE(s.snapshot()->recent_fills[0].price, m("4.20"));
}
TEST(TradingMatching, SessionEndEarlyCloseAndExpiryCancelBeforeFill) {
  for (const auto date : {md::Date{2026, 9, 22}, md::Date{2026, 11, 27}}) {
    ScriptedMarket f;
    f.contract.expiry = {2026, 12, 18};
    f.time = md::new_york_to_utc(date, 10, 0);
    TradingSession s(roomy(), f.time);
    f.seed(s);
    s.submit(f.limit("rest", 1, "4.10"), f.time);
    const auto end = md::new_york_to_utc(date, md::regular_close_hour(date), 15);
    EXPECT_EQ(s.snapshot()->recent_orders[0].day_end, end);
    f.time = end;
    ++f.observation;
    s.on_quotes({f.quote("4", "4.10")}, {f.valuation()}, f.time);
    EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::DAY_END);
    EXPECT_TRUE(s.snapshot()->recent_fills.empty());
    EXPECT_EQ(s.submit(f.market("closed"), f.time).decision.code, Reason::SESSION_CLOSED);
  }
  ScriptedMarket f;
  f.contract.expiry = {2026, 9, 22};
  TradingSession s(roomy(), f.time);
  f.seed(s);
  s.submit(f.limit("rest", 1, "4.10"), f.time);
  s.on_quotes({}, {}, f.contract.expiry_time());
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::EXPIRED);
}
TEST(TradingMatching, RestingOrdersDoNotConsumeObservationsPredatingAcceptance) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s, "4", "4.60");
  const auto initial = f.time;
  f.time += 30 * md::kNanosPerSecond;
  s.submit(f.limit("rest", 1, "4.20"), f.time);
  auto late = f.quote("4", "4.20", 2);
  late.observation = 2;
  late.time = initial + 10 * md::kNanosPerSecond;
  s.on_quotes({late}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  // A new order can still take a fresh cached far side without time travel.
  s.submit(f.market("incoming"), f.time);
  ASSERT_EQ(s.snapshot()->recent_fills.size(), 1);
  EXPECT_EQ(s.snapshot()->recent_fills[0].order_id, 2);
  late.observation = 3;
  late.time = f.time;
  s.on_quotes({late}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::Filled);
}
TEST(TradingMatching, FillUsesCurrentFarSideBandAndKeepsPartialQuantityOnRiskChange) {
  ScriptedMarket f;
  auto config = roomy();
  config.limits.price_band_relative = 0;
  config.limits.price_band_absolute = m("0.15");
  TradingSession s(config, f.time);
  f.seed(s, "4", "4.20", 1);
  s.submit(f.limit("partial", 2), f.time);
  f.next();
  // Ask still crosses, but the widened spread puts the far side outside band.
  s.on_quotes({f.quote("3", "4.20", 10)}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(s.snapshot()->recent_orders[0].filled_quantity, 1);
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::RISK_CHANGED);
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.actual, 0.6);
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 1);

  TradingSession favorable(roomy(), f.time);
  f.seed(favorable, "4", "4.60");
  favorable.submit(f.limit("improve", 1, "4.20"), f.time);
  f.next();
  favorable.on_quotes({f.quote("1", "1.20")}, {f.valuation()}, f.time);
  ASSERT_EQ(favorable.snapshot()->recent_fills.size(), 1);
  EXPECT_EQ(favorable.snapshot()->recent_fills[0].price, m("1.20"));
}
TEST(TradingMatching, LateDataAndBatchRejectionAreAtomic) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s, "4", "4.60");
  s.submit(f.limit("rest", 1, "4.20"), f.time);
  auto old = f.quote("4", "4.10");
  f.next();
  old.observation = 99;
  old.time -= md::kNanosPerSecond;
  s.on_quotes({old}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  const auto before = s.snapshot_json();
  auto future = f.quote(); future.time += md::kNanosPerSecond;
  EXPECT_THROW(s.on_quotes({future}, {}, f.time), TradingError);
  EXPECT_EQ(s.snapshot_json(), before);
  EXPECT_THROW(s.on_quotes({f.quote(), f.quote()}, {}, f.time), TradingError);
  EXPECT_EQ(s.snapshot_json(), before);
  EXPECT_THROW(s.on_quotes({}, {}, f.time - 2 * md::kNanosPerSecond), TradingError);
}
}  // namespace
}  // namespace openport::trading

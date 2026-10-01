#include <gtest/gtest.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
SessionConfig config() {
  SessionConfig c;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.limits.max_daily_loss = m("100");
  c.limits.price_band_absolute = m("5");
  return c;
}
OrderRequest spread(std::string id, const ScriptedMarket& a, const ScriptedMarket& b, bool close,
                    Quantity quantity = 1, std::optional<Money> limit = {}) {
  OrderRequest r;
  r.client_order_id = std::move(id);
  r.quantity = quantity;
  r.type = limit ? OrderType::Limit : OrderType::Market;
  r.tif = limit ? TimeInForce::Day : TimeInForce::Ioc;
  r.limit_price = limit;
  r.legs = {{a.symbol(), close ? Side::Sell : Side::Buy, 2}, {b.symbol(), close ? Side::Buy : Side::Sell, 1}};
  return r;
}

TEST(TradingKill, ClosingLimitWorksUnderManualAndDailyLossLatch) {
  for (const bool daily : {false, true}) {
    ScriptedMarket f;
    auto c = config();
    c.rules.defined_risk = true;
    TradingSession s(c, f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
    if (daily) {
      f.next();
      s.on_quotes({f.quote("1", "1.20")}, {f.valuation()}, f.time);
    } else {
      s.trip_kill("manual", f.time);
    }
    ASSERT_TRUE(s.snapshot()->risk.kill_latched);
    EXPECT_EQ(s.submit(f.market("increase"), f.time).decision.code, Reason::KILL_SWITCH);
    EXPECT_EQ(s.submit(f.market("flip", 3, Side::Sell), f.time).decision.code, Reason::KILL_SWITCH);
    const auto close = s.submit(f.limit("close", 2, daily ? "1.50" : "4.50", Side::Sell), f.time);
    ASSERT_TRUE(close.decision.ok());
    ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
    // Only part fills on the next quote; the remainder still counts as a close.
    f.next();
    s.on_quotes({f.quote(daily ? "1.50" : "4.50", daily ? "1.70" : "4.70", 1)}, {f.valuation()}, f.time);
    ASSERT_EQ(s.snapshot()->positions.size(), 1U);
    EXPECT_EQ(s.snapshot()->positions[0].position.quantity, 1);
    f.next();
    s.on_quotes({f.quote(daily ? "1.50" : "4.50", daily ? "1.70" : "4.70", 1)}, {f.valuation()}, f.time);
    EXPECT_TRUE(s.snapshot()->positions.empty());
    EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  }
}

TEST(TradingKill, TripKeepsClosingOrdersAndBothBracketExits) {
  for (const bool daily : {false, true}) {
    for (const bool stop : {false, true}) {
      ScriptedMarket f;
      TradingSession s(config(), f.time);
      f.seed(s);
      auto entry = f.market("entry", 2);
      entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("1")}, {}},
                              ExitSpec{{}, m("5")}};
      ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
      ASSERT_TRUE(s.submit(f.limit("manual-close", 1, "5.50", Side::Sell), f.time).decision.ok());
      ASSERT_TRUE(s.submit(f.limit("opening", 1, "3.50"), f.time).decision.ok());
      if (daily) {
        f.next();
        s.on_quotes({f.quote("2", "2.20")}, {f.valuation()}, f.time);
      } else {
        s.trip_kill("manual", f.time);
      }
      ASSERT_TRUE(s.snapshot()->risk.kill_latched);
      ASSERT_EQ(s.snapshot()->open_orders.size(), 3U);
      s.set_limits(s.config().limits, f.time);
      EXPECT_EQ(s.snapshot()->open_orders.size(), 3U);
      EXPECT_EQ(s.snapshot()->recent_orders.back().reason.code, Reason::KILL_SWITCH);
      f.next();
      s.on_quotes({f.quote(stop ? "1" : "5", stop ? "1.20" : "5.20")}, {f.valuation()}, f.time);
      EXPECT_TRUE(s.snapshot()->positions.empty());
      EXPECT_EQ(s.snapshot()->recent_fills.size(), 2U);
      // A surviving manual close cannot flip the position after an exit filled.
      f.next();
      s.on_quotes({f.quote("5.50", "5.70")}, {f.valuation()}, f.time);
      EXPECT_TRUE(s.snapshot()->positions.empty());
      EXPECT_EQ(s.snapshot()->recent_fills.size(), 2U);
    }
  }
}

TEST(TradingKill, FlattenAfterDailyLossClosesEveryPosition) {
  ScriptedMarket a, b;
  b.contract.strike += 10;
  auto c = config();
  c.rules.defined_risk = true;
  TradingSession s(c, a.time);
  a.seed(s);
  b.seed(s);
  ASSERT_TRUE(s.submit(spread("open", a, b, false), a.time).decision.ok());
  a.next(); b.next();
  s.on_quotes({a.quote("1", "1.20"), b.quote()}, {a.valuation(), b.valuation()}, a.time);
  ASSERT_TRUE(s.snapshot()->risk.kill_latched);
  ASSERT_EQ(s.snapshot()->positions.size(), 2U);
  ASSERT_TRUE(s.close_positions({}, a.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
  // The short closes with one of the two longs as one order (two fills); the other long alone.
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 5U);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
}

TEST(TradingKill, ClosingSpreadWorksButOpeningOrMixedLegsDoNot) {
  for (const bool daily : {false, true}) {
    ScriptedMarket a, b;
    b.contract.strike += 10;
    TradingSession s(config(), a.time);
    a.seed(s); b.seed(s);
    ASSERT_TRUE(s.submit(spread("open", a, b, false), a.time).decision.ok());
    if (daily) {
      a.next(); b.next();
      s.on_quotes({a.quote("1", "1.20"), b.quote()}, {a.valuation(), b.valuation()}, a.time);
    } else {
      s.trip_kill("manual", a.time);
    }
    EXPECT_EQ(s.submit(spread("increase", a, b, false), a.time).decision.code, Reason::KILL_SWITCH);
    auto mixed = spread("mixed", a, b, true);
    mixed.legs[1].side = Side::Sell;
    EXPECT_EQ(s.submit(mixed, a.time).decision.code, Reason::KILL_SWITCH);
    const auto close = s.submit(spread("close", a, b, true, 1, m(daily ? "1.00" : "-4.00")), a.time);
    ASSERT_TRUE(close.decision.ok());
    ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
    a.next(); b.next();
    s.on_quotes({a.quote(daily ? "2" : "5", daily ? "2.20" : "5.20"), b.quote()},
                {a.valuation(), b.valuation()}, a.time);
    EXPECT_TRUE(s.snapshot()->positions.empty());
    EXPECT_EQ(s.snapshot()->recent_fills.size(), 4U);
    EXPECT_EQ(s.submit(spread("new", a, b, false), a.time).decision.code, Reason::KILL_SWITCH);
  }
}

TEST(TradingKill, WorkingSingleAndMultiLegOrdersShareClosingCapacity) {
  for (const bool combo_first : {false, true}) {
    ScriptedMarket a, b;
    b.contract.strike += 10;
    TradingSession s(config(), a.time);
    a.seed(s); b.seed(s);
    ASSERT_TRUE(s.submit(spread("open", a, b, false, 2), a.time).decision.ok());
    s.trip_kill("manual", a.time);
    const auto combo = spread("combo", a, b, true, 1, m("-5"));
    const auto single = a.limit("single", 2, "5", Side::Sell);
    ASSERT_TRUE(s.submit(combo_first ? combo : single, a.time).decision.ok());
    ASSERT_TRUE(s.submit(combo_first ? single : combo, a.time).decision.ok());
    EXPECT_EQ(s.submit(a.limit("too-many", 1, "5", Side::Sell), a.time).decision.code, Reason::KILL_SWITCH);
    EXPECT_EQ(s.submit(spread("too-many-combos", a, b, true, 1, m("-5")), a.time).decision.code, Reason::KILL_SWITCH);
    a.next(); b.next();
    s.on_quotes({a.quote("5", "5.20"), b.quote()}, {a.valuation(), b.valuation()}, a.time);
    ASSERT_EQ(s.snapshot()->positions.size(), 1U);
    EXPECT_EQ(s.snapshot()->positions[0].position.contract.osi_symbol(), b.symbol());
    EXPECT_EQ(s.snapshot()->positions[0].position.quantity, -1);
    EXPECT_EQ(s.snapshot()->recent_fills.size(), 5U);
  }
}

TEST(TradingKill, TripCancelsExcessWorkingClosesAndCannotFlip) {
  ScriptedMarket f;
  TradingSession s(config(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  for (const auto id : {"close-1", "close-2", "excess"})
    ASSERT_TRUE(s.submit(f.limit(id, 1, "5", Side::Sell), f.time).decision.ok());
  s.trip_kill("manual", f.time);
  ASSERT_EQ(s.snapshot()->open_orders.size(), 2U);
  const auto id = s.snapshot()->open_orders.front().id;
  OrderChange change;
  change.quantity = 2;
  EXPECT_EQ(s.modify(id, change, f.time).decision.code, Reason::KILL_SWITCH);
  f.next();
  s.on_quotes({f.quote("5", "5.20")}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 3U);
}

TEST(TradingKill, TripPreservesCloseAheadOfAnOpeningOrderOnTheSameSide) {
  ScriptedMarket f;
  TradingSession s(config(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("close", 1, "5", Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("flip", 3, "5", Side::Sell), f.time).decision.ok());
  s.trip_kill("manual", f.time);
  ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
  EXPECT_EQ(s.snapshot()->open_orders[0].request.client_order_id, "close");
  f.next();
  s.on_quotes({f.quote("5", "5.20")}, {f.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_EQ(s.snapshot()->positions[0].position.quantity, 1);
}

TEST(TradingKill, SystemAutoCloseStillWorksWhileLatched) {
  ScriptedMarket f;
  f.contract.expiry = {2026, 9, 22};
  auto c = config();
  c.rules.expiry_cutoff = 5 * md::kNanosPerMinute;
  TradingSession s(c, f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  s.trip_kill("manual", f.time);
  f.time = f.contract.last_trade_time() - c.rules.expiry_cutoff;
  ++f.observation;
  s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_TRUE(s.snapshot()->recent_orders.back().system);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
}

TEST(TradingKill, ClosingStillChecksDefinedRiskAndEvaluation) {
  ScriptedMarket a, b;
  b.contract.strike += 10;
  auto c = config();
  c.rules.defined_risk = true;
  c.rules.profit_target = m("100");
  TradingSession s(c, a.time);
  a.seed(s); b.seed(s);
  auto equal = spread("open", a, b, false);
  equal.legs[0].ratio = 1;
  equal.quantity = 2;
  ASSERT_TRUE(s.submit(equal, a.time).decision.ok());
  s.trip_kill("manual", a.time);
  EXPECT_EQ(s.submit(a.market("uncover", 1, Side::Sell), a.time).decision.code, Reason::DEFINED_RISK);
  a.next(); b.next();
  s.on_quotes({a.quote("6", "6.20", 1), b.quote()}, {a.valuation(), b.valuation()}, a.time);
  ASSERT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  // The rule closes only part of the long on this quote, but the closed attempt
  // still refuses user orders even if they would otherwise reduce it.
  EXPECT_EQ(s.submit(a.market("decided", 1, Side::Sell), a.time).decision.code, Reason::EVALUATION_CLOSED);
}
}  // namespace
}  // namespace openport::trading

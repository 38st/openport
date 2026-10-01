#include <gtest/gtest.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
SessionConfig config(std::int64_t latency = 0, std::int64_t impact = 0, std::int64_t slip = 0) {
  SessionConfig result;
  result.limits.aggregate = {1e9, 1e9};
  result.limits.per_underlying = {1e9, 1e9};
  result.rules.fill_latency_ms = latency;
  result.rules.impact_ticks = impact;
  result.rules.slippage_ticks = slip;
  return result;
}
void next(TradingSession& session, ScriptedMarket& market, std::string_view bid = "4.00",
          std::string_view ask = "4.20", Quantity size = 10) {
  market.next();
  session.on_quotes({market.quote(bid, ask, size)}, {market.valuation()}, market.time);
}
const Order& order(const TradingSession& session, OrderId id = 1) { return session.snapshot()->recent_orders.at(id - 1); }

TEST(TradingFills, ImpactSweepsBlocksAndSharesBudgetsAcrossOrdersAndSides) {
  ScriptedMarket market;
  TradingSession session(config(0, 1, 1), market.time);
  market.seed(session, "4.00", "4.20", 2);
  ASSERT_TRUE(session.submit(market.market("buy", 5), market.time).decision.ok());
  auto fills = session.snapshot()->recent_fills;
  ASSERT_EQ(fills.size(), 3U);
  EXPECT_EQ(fills[0].quantity, 2);
  EXPECT_EQ(fills[0].price, m("4.30"));
  EXPECT_EQ(fills[1].quantity, 2);
  EXPECT_EQ(fills[1].price, m("4.40"));
  EXPECT_EQ(fills[2].quantity, 1);
  EXPECT_EQ(fills[2].price, m("4.50"));
  EXPECT_EQ(order(session).filled_notional, m("21.90"));
  ASSERT_TRUE(session.submit(market.market("more", 1), market.time).decision.ok());
  EXPECT_EQ(session.snapshot()->recent_fills.back().price, m("4.50"));
  ASSERT_TRUE(session.submit(market.market("sell", 3, Side::Sell), market.time).decision.ok());
  fills = session.snapshot()->recent_fills;
  EXPECT_EQ(fills[fills.size() - 2].price, m("3.90"));
  EXPECT_EQ(fills.back().price, m("3.80"));
  EXPECT_EQ(session.snapshot()->account.fees, m("5.85"));
  // Reconfirming the same observation advances time without refreshing depth.
  market.time += md::kNanosPerSecond;
  session.on_quotes({market.quote("4.00", "4.20", 2)}, {market.valuation()}, market.time);
  ASSERT_TRUE(session.submit(market.market("same", 1), market.time).decision.ok());
  EXPECT_EQ(session.snapshot()->recent_fills.back().price, m("4.60"));
  next(session, market, "4.00", "4.20", 2);
  ASSERT_TRUE(session.submit(market.market("fresh", 1), market.time).decision.ok());
  EXPECT_EQ(session.snapshot()->recent_fills.back().price, m("4.30"));
}

TEST(TradingFills, ImpactLimitsRestTheirRemainderOrCancelItForIoc) {
  for (const auto tif : {TimeInForce::Day, TimeInForce::Gtc, TimeInForce::Ioc}) {
    ScriptedMarket market;
    TradingSession session(config(0, 1), market.time);
    market.seed(session, "4.00", "4.20", 2);
    ASSERT_TRUE(session.submit(market.limit("limit", 5, "4.30", Side::Buy, tif), market.time).decision.ok());
    EXPECT_EQ(order(session).filled_quantity, 4);
    EXPECT_EQ(order(session).remaining(), 1);
    if (tif == TimeInForce::Ioc) {
      EXPECT_EQ(order(session).reason.code, Reason::IOC_REMAINDER);
    } else {
      EXPECT_EQ(order(session).status, OrderStatus::PartiallyFilled);
    }
    next(session, market, "4.00", "4.20", 2);
    EXPECT_EQ(order(session).filled_quantity, tif == TimeInForce::Ioc ? 4 : 5);
  }
}

TEST(TradingFills, ImpactUsesDisplayedTickTierAndFloorsSalesAtZero) {
  ScriptedMarket market;
  TradingSession session(config(0, 1, 1), market.time);
  market.seed(session, "2.85", "2.95", 1);
  ASSERT_TRUE(session.submit(market.market("tier", 3), market.time).decision.ok());
  const auto fills = session.snapshot()->recent_fills;
  ASSERT_EQ(fills.size(), 3U);
  EXPECT_EQ(fills[0].price, m("3.00"));
  EXPECT_EQ(fills[1].price, m("3.05"));
  EXPECT_EQ(fills[2].price, m("3.10"));
  next(session, market, "0.05", "0.10", 1);
  ASSERT_TRUE(session.submit(market.market("floor", 3, Side::Sell), market.time).decision.ok());
  EXPECT_EQ(session.snapshot()->recent_fills.back().price, Money{});
}

TEST(TradingFills, PreviewUsesDisplayedOrPreviouslyConsumedDepthWithoutChangingIt) {
  ScriptedMarket market;
  TradingSession session(config(0, 1), market.time);
  PreviewMarket supplied;
  supplied.contracts = {market.contract};
  supplied.quotes = {market.quote("4.00", "4.20", 1)};
  supplied.valuations = {market.valuation()};
  const auto request = market.market("preview");
  const auto empty = session.snapshot_json();
  auto preview = session.preview(request, market.time, 0.5, {}, {}, supplied);
  ASSERT_TRUE(preview.decision.ok());
  EXPECT_EQ(preview.max_loss, m("420.65"));
  EXPECT_EQ(session.snapshot_json(), empty);
  market.seed(session, "4.00", "4.20", 1);
  ASSERT_TRUE(session.submit(market.market("consume"), market.time).decision.ok());
  const auto before = session.snapshot_json();
  preview = session.preview(request, market.time, 0.5, {}, {}, supplied);
  ASSERT_TRUE(preview.decision.ok());
  EXPECT_EQ(preview.max_loss, m("430.65"));
  EXPECT_EQ(session.snapshot_json(), before);
}

TEST(TradingFills, ImpactNeverSuppliesDepthForInvalidBooks) {
  ScriptedMarket market;
  TradingSession session(config(1000, 1), market.time);
  market.seed(session);
  ASSERT_TRUE(session.submit(market.market("wait", 20), market.time).decision.ok());
  next(session, market, "4.00", "4.20", 0);
  next(session, market, "4.30", "4.20", 2);
  EXPECT_TRUE(session.snapshot()->recent_fills.empty());
  EXPECT_TRUE(order(session).open());
  session.cancel(1, market.time);
}

TEST(TradingFills, ImpactSellLimitStopsBeforeWorseBlock) {
  ScriptedMarket market;
  TradingSession session(config(0, 1), market.time);
  market.seed(session, "4.00", "4.20", 2);
  ASSERT_TRUE(session.submit(market.limit("sell", 5, "3.90", Side::Sell), market.time).decision.ok());
  EXPECT_EQ(order(session).filled_quantity, 4);
  EXPECT_EQ(order(session).status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(order(session).filled_notional, m("15.80"));
  next(session, market, "4.00", "4.20", 1);
  EXPECT_EQ(order(session).filled_quantity, 5);
  EXPECT_EQ(session.snapshot()->recent_fills.back().price, m("4.00"));
}

TEST(TradingFills, ImpactRechecksRiskBeforeConsumingEachTier) {
  ScriptedMarket market;
  auto settings = config(0, 10);
  settings.limits.price_band_absolute = m("0.50");
  settings.limits.price_band_relative = 0;
  TradingSession session(settings, market.time);
  market.seed(session, "4.00", "4.20", 1);
  ASSERT_TRUE(session.submit(market.market("risk", 3), market.time).decision.ok());
  EXPECT_EQ(order(session).filled_quantity, 1);
  EXPECT_EQ(order(session).reason.code, Reason::RISK_CHANGED);
  EXPECT_NE(order(session).reason.message.find("PRICE_BAND"), std::string::npos);
}

TEST(TradingFills, LatencyUsesFirstValidQuoteAtOrAfterDeadlineNotTransactionTime) {
  ScriptedMarket market;
  TradingSession session(config(1500), market.time);
  market.seed(session);
  const auto accepted = market.time;
  ASSERT_TRUE(session.submit(market.market("delayed", 3), market.time).decision.ok());
  EXPECT_EQ(order(session).status, OrderStatus::Working);
  next(session, market, "4.10", "4.30");
  EXPECT_TRUE(session.snapshot()->recent_fills.empty());
  session.on_quotes({}, {}, accepted + 1500 * (md::kNanosPerSecond / 1000));
  EXPECT_TRUE(session.snapshot()->recent_fills.empty());
  next(session, market, "4.10", "4.30", 0);
  EXPECT_TRUE(order(session).open());
  next(session, market, "4.20", "4.40", 2);
  ASSERT_EQ(session.snapshot()->recent_fills.size(), 1U);
  const auto fill = session.snapshot()->recent_fills.front();
  EXPECT_EQ(fill.price, m("4.40"));
  EXPECT_EQ(fill.quote_time, market.time);
  EXPECT_EQ(fill.quantity, 2);
  EXPECT_EQ(order(session).remaining(), 1);
  EXPECT_EQ(order(session).reason.code, Reason::IOC_REMAINDER);
  next(session, market);
  EXPECT_EQ(order(session).filled_quantity, 2);
}

TEST(TradingFills, LatencyBoundaryIsInclusiveAndReconfirmedQuotesKeepTheirBudget) {
  ScriptedMarket market;
  TradingSession session(config(1000), market.time);
  market.seed(session, "4.00", "4.20", 2);
  ASSERT_TRUE(session.submit(market.market("first", 2), market.time).decision.ok());
  market.time += md::kNanosPerSecond;
  session.on_quotes({market.quote("4.00", "4.20", 2)}, {market.valuation()}, market.time);
  EXPECT_EQ(order(session).filled_quantity, 2);
  ASSERT_TRUE(session.submit(market.market("second"), market.time).decision.ok());
  market.time += md::kNanosPerSecond;
  session.on_quotes({market.quote("4.00", "4.20", 2)}, {market.valuation()}, market.time);
  EXPECT_EQ(order(session, 2).filled_quantity, 0);
  EXPECT_EQ(order(session, 2).reason.code, Reason::IOC_REMAINDER);
}

TEST(TradingFills, LatencyLimitsWaitAndKeepWorkingAfterPartialFills) {
  ScriptedMarket market;
  TradingSession session(config(1000), market.time);
  market.seed(session);
  ASSERT_TRUE(session.submit(market.limit("rest", 3), market.time).decision.ok());
  next(session, market, "4.10", "4.30", 1);
  EXPECT_EQ(order(session).status, OrderStatus::Working);
  next(session, market, "4.00", "4.20", 1);
  EXPECT_EQ(order(session).filled_quantity, 1);
  next(session, market, "3.90", "4.10", 2);
  EXPECT_EQ(order(session).status, OrderStatus::Filled);
  EXPECT_EQ(order(session).filled_notional, m("12.40"));
}

TEST(TradingFills, LatencyCancellationAndKillSwitchPreventPendingEntries) {
  for (const bool kill : {false, true}) {
    ScriptedMarket market;
    TradingSession session(config(1000), market.time);
    market.seed(session);
    ASSERT_TRUE(session.submit(market.market("pending"), market.time).decision.ok());
    if (kill) session.trip_kill("test", market.time);
    else session.cancel(1, market.time);
    next(session, market);
    EXPECT_TRUE(session.snapshot()->recent_fills.empty());
    EXPECT_EQ(order(session).reason.code, kill ? Reason::KILL_SWITCH : Reason::USER_CANCEL);
  }
}

TEST(TradingFills, LatencyDayExpiresBeforeExecutionAndGtcWaitsForNextRegularSession) {
  for (const auto tif : {TimeInForce::Day, TimeInForce::Gtc, TimeInForce::Ioc}) {
    ScriptedMarket market;
    market.time = md::new_york_to_utc({2026, 9, 22}, 16, 14, 59);
    TradingSession session(config(2000), market.time);
    market.seed(session);
    const auto request = tif == TimeInForce::Ioc ? market.market("close") : market.limit("close", 1, "4.20", Side::Buy, tif);
    ASSERT_TRUE(session.submit(request, market.time).decision.ok());
    next(session, market);
    EXPECT_TRUE(session.snapshot()->recent_fills.empty());
    if (tif != TimeInForce::Gtc) {
      EXPECT_EQ(order(session).reason.code, Reason::DAY_END);
    }
    market.time = md::new_york_to_utc({2026, 9, 23}, 9, 30);
    ++market.observation;
    session.on_quotes({market.quote()}, {market.valuation()}, market.time);
    EXPECT_EQ(order(session).filled_quantity, tif == TimeInForce::Gtc ? 1 : 0);
  }
}

TEST(TradingFills, LatencyCannotFillAtExpiryOrAutoCloseDeadline) {
  for (const bool cutoff : {false, true}) {
    ScriptedMarket market;
    const auto deadline = market.contract.last_trade_time() - (cutoff ? md::kNanosPerMinute : 0);
    market.time = deadline - md::kNanosPerSecond;
    auto settings = config(2000);
    settings.rules.expiry_cutoff = cutoff ? md::kNanosPerMinute : 0;
    TradingSession session(settings, market.time);
    market.seed(session);
    ASSERT_TRUE(session.submit(market.limit("expiry", 1, "4.20", Side::Buy, TimeInForce::Gtc), market.time).decision.ok());
    next(session, market);
    EXPECT_EQ(order(session).reason.code, cutoff ? Reason::EXPIRY_CUTOFF : Reason::EXPIRED);
    EXPECT_TRUE(session.snapshot()->recent_fills.empty());
  }
}

TEST(TradingFills, BracketsStartAtEntryFillAndStopsStartLatencyAtTrigger) {
  for (const bool stop : {false, true}) {
    ScriptedMarket market;
    TradingSession session(config(1000), market.time);
    market.seed(session);
    auto entry = market.market("entry", 2);
    entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.50")}, {}},
                            ExitSpec{{}, m("5.00")}};
    ASSERT_TRUE(session.submit(entry, market.time).decision.ok());
    EXPECT_EQ(session.snapshot()->recent_orders.size(), 1U);
    next(session, market);
    EXPECT_EQ(order(session).filled_quantity, 2);
    EXPECT_EQ(order(session, 3).accepted_at, market.time);
    if (stop) {
      next(session, market, "3.50", "3.70");
      EXPECT_EQ(order(session, 2).status, OrderStatus::Working);
      EXPECT_EQ(order(session, 2).triggered_at, market.time);
      EXPECT_TRUE(order(session, 3).open());
      session.trip_kill("reduce only", market.time);
      next(session, market, "3.00", "3.20", 1);
      EXPECT_EQ(order(session, 2).filled_quantity, 1);
      EXPECT_EQ(session.snapshot()->recent_fills.back().price, m("3.00"));
      // The remainder re-arms, and the bid still at the stop triggers it again: its
      // delay starts over, and the target stays for the contract still held.
      EXPECT_EQ(order(session, 2).status, OrderStatus::Working);
      EXPECT_EQ(order(session, 2).triggered_at, market.time);
      EXPECT_EQ(order(session, 3).request.quantity, 1);
      EXPECT_TRUE(order(session, 3).open());
      next(session, market, "2.90", "3.10", 1);
      EXPECT_EQ(order(session, 2).status, OrderStatus::Filled);
      EXPECT_EQ(session.snapshot()->recent_fills.back().price, m("2.90"));
    } else {
      next(session, market, "5.00", "5.20");
      EXPECT_EQ(order(session, 3).filled_quantity, 2);
      EXPECT_TRUE(session.snapshot()->positions.empty());
    }
    EXPECT_EQ(order(session, stop ? 3 : 2).reason.code, Reason::OCO_FILLED);
  }
}

TEST(TradingFills, TriggeredEntryWaitsAndCanBeCancelledBeforeExecution) {
  ScriptedMarket market;
  TradingSession session(config(2000), market.time);
  market.seed(session);
  auto request = market.market("stop");
  request.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrAbove, m("4.30")};
  ASSERT_TRUE(session.submit(request, market.time).decision.ok());
  next(session, market, "4.10", "4.30");
  EXPECT_EQ(order(session).triggered_at, market.time);
  next(session, market, "4.20", "4.40");
  EXPECT_TRUE(session.snapshot()->recent_fills.empty());
  session.cancel(1, market.time);
  next(session, market);
  EXPECT_EQ(order(session).reason.code, Reason::USER_CANCEL);
}

TEST(TradingFills, SystemAutoCloseWaitsWithoutCreatingDuplicatePendingOrders) {
  ScriptedMarket market;
  auto settings = config(1000);
  settings.rules.expiry_cutoff = md::kNanosPerMinute;
  market.time = market.contract.last_trade_time() - 2 * md::kNanosPerMinute;
  TradingSession session(settings, market.time);
  market.seed(session);
  ASSERT_TRUE(session.submit(market.market("entry"), market.time).decision.ok());
  next(session, market);
  market.time = market.contract.last_trade_time() - md::kNanosPerMinute;
  ++market.observation;
  session.on_quotes({market.quote()}, {market.valuation()}, market.time);
  ASSERT_EQ(session.snapshot()->recent_orders.size(), 2U);
  EXPECT_TRUE(order(session, 2).system);
  EXPECT_EQ(order(session, 2).status, OrderStatus::Working);
  session.on_quotes({}, {}, market.time);
  EXPECT_EQ(session.snapshot()->recent_orders.size(), 2U);
  next(session, market);
  EXPECT_TRUE(session.snapshot()->positions.empty());
  EXPECT_EQ(order(session, 2).filled_quantity, 1);
}

}  // namespace
}  // namespace openport::trading

#include <gtest/gtest.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
SessionConfig config() {
  SessionConfig c;
  c.initial_cash = m("10000");
  c.rules.max_drawdown = m("1000");
  c.rules.buying_power = true;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.limits.price_band_absolute = m("20");
  return c;
}
OrderRequest spread(const ScriptedMarket& f, const ScriptedMarket& wing, std::string client, Quantity units) {
  OrderRequest order = f.market(std::move(client), units);
  order.symbol.clear();
  order.legs = {{f.symbol(), Side::Buy, 1}, {wing.symbol(), Side::Sell, 1}};
  return order;
}

TEST(TradingBreach, TheSoftFloorHasItsOwnLevelsBesideThePlanFloors) {
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  Ledger ledger(m("10000")); ledger.trade_stock("SPY", 100, m("100"), {});
  // 100 shares lose 1,000 at -10% (the plan floor) and 500 at -5% (the soft floor).
  const auto both = breach_risk(ledger, {}, m("10000"), m("9000"), m("9500"), time,
      md::kNanosPerMinute, {{"SPY", 100}}, {{"SPY", 0.0004}});
  ASSERT_EQ(both.underlyings.size(), 1U);
  const auto& item = both.underlyings.front();
  ASSERT_TRUE(item.down); ASSERT_TRUE(item.soft_down);
  EXPECT_NEAR(item.down->percent, -10, 1e-7);
  EXPECT_NEAR(item.soft_down->percent, -5, 1e-7);
  EXPECT_NEAR(item.soft_down->points, -5, 1e-7);
  ASSERT_TRUE(item.soft_down->touch_probability);
  EXPECT_GT(*item.soft_down->touch_probability, *item.down->touch_probability);
  EXPECT_FALSE(item.up); EXPECT_FALSE(item.soft_up);
  // Without a plan floor the levels are the soft floor's, under both names.
  const auto soft = breach_risk(ledger, {}, m("10000"), {}, m("9500"), time,
      md::kNanosPerMinute, {{"SPY", 100}}, {{"SPY", 0.0004}});
  ASSERT_TRUE(soft.underlyings.front().down); ASSERT_TRUE(soft.underlyings.front().soft_down);
  EXPECT_NEAR(soft.underlyings.front().down->percent, -5, 1e-7);
  EXPECT_EQ(soft.underlyings.front().soft_down->percent, soft.underlyings.front().down->percent);
  // Without a soft floor there are none.
  const auto plan = breach_risk(ledger, {}, m("10000"), m("9000"), {}, time,
      md::kNanosPerMinute, {{"SPY", 100}}, {{"SPY", 0.0004}});
  ASSERT_TRUE(plan.underlyings.front().down);
  EXPECT_FALSE(plan.underlyings.front().soft_down);
}
TEST(TradingBreach, AccountsWithBothFloorsPublishTheSoftFloorsLevels) {
  ScriptedMarket f;
  SessionConfig c;
  c.initial_cash = m("10000");
  c.rules.max_drawdown = m("1000");
  c.limits.price_band_absolute = m("20");
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  Guardrails g; g.soft_floor = m("9700"); s.set_guardrails(g, f.time);
  const auto risk = s.breach({{"SPX", 0.0004}});
  ASSERT_EQ(risk.underlyings.size(), 1U);
  const auto& item = risk.underlyings.front();
  ASSERT_TRUE(item.down); ASSERT_TRUE(item.soft_down);
  // The soft floor is nearer: a smaller fall reaches it.
  EXPECT_LT(item.down->percent, item.soft_down->percent);
  EXPECT_LT(item.soft_down->percent, 0);
}
TEST(TradingPreview, BuyingPowerFitIsReportedApartFromFloorFit) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  // A call costs 420.65: half the 1,000 floor room fits one, and 10,000 of
  // buying power fits 23 (24 would cost 10,095.60).
  const auto p = s.preview(f.market("size"), f.time);
  EXPECT_EQ(p.max_units, 1);
  EXPECT_EQ(p.max_units_buying_power, 23);
  EXPECT_EQ(p.max_units_floor, 1);
  // The whole room fits two, still within buying power.
  const auto whole = s.preview(f.market("size"), f.time, 1);
  EXPECT_EQ(whole.max_units, 2);
  EXPECT_EQ(whole.max_units_floor, 2);
  // Without a floor, sizing is buying power and the limits alone.
  auto c = config(); c.rules.max_drawdown = {};
  TradingSession practice(c, f.time); f.seed(practice);
  const auto free = practice.preview(f.market("size"), f.time);
  EXPECT_EQ(free.max_units, 23);
  EXPECT_EQ(free.max_units_buying_power, 23);
  EXPECT_FALSE(free.max_units_floor);
}
TEST(TradingPreview, SizingIsNullWhenUnavailableAndZeroWhenNothingFits) {
  ScriptedMarket f; auto c = config(); c.initial_cash = m("300");
  TradingSession poor(c, f.time); f.seed(poor);
  // One call needs 420.65 of the 300 available: no size fits.
  const auto short_of_power = poor.preview(f.market("poor"), f.time);
  EXPECT_EQ(short_of_power.decision.code, Reason::BUYING_POWER);
  EXPECT_EQ(short_of_power.max_units, 0);
  EXPECT_EQ(short_of_power.max_units_buying_power, 0);
  EXPECT_EQ(short_of_power.max_units_floor, 0);
  TradingSession s(config(), f.time); f.seed(s);
  // An off-tick limit is refused at any size: sizing is unavailable, not zero.
  const auto off_tick = s.preview(f.limit("tick", 1, "4.01"), f.time);
  EXPECT_EQ(off_tick.decision.code, Reason::INVALID_TICK);
  EXPECT_FALSE(off_tick.max_units);
  EXPECT_FALSE(off_tick.max_units_buying_power);
  EXPECT_FALSE(off_tick.max_units_floor);
  // So is a paused account's.
  s.trip_kill("pause", f.time);
  const auto paused = s.preview(f.market("paused"), f.time);
  EXPECT_EQ(paused.decision.code, Reason::KILL_SWITCH);
  EXPECT_FALSE(paused.max_units);
  s.reset_kill("resume", f.time);
  // And a stale book's: the loss cannot be projected.
  f.time += 61 * md::kNanosPerSecond;
  const auto stale = s.preview(f.market("stale"), f.time);
  EXPECT_EQ(stale.decision.code, Reason::STALE_QUOTE);
  EXPECT_FALSE(stale.max_units);
}
TEST(TradingPreview, HeldExitsShowBuyingPowerOnceAcceptedNotAsIfATargetFilled) {
  ScriptedMarket f, wing; wing.contract.strike = 5010;
  TradingSession s(config(), f.time); f.seed(s); wing.seed(s, "2", "2.20");
  ASSERT_TRUE(s.submit(spread(f, wing, "open", 2), f.time).decision.ok());
  // A target far from the market: the pair rests, holding its fees.
  OrderRequest exits = spread(f, wing, "exits", 2);
  for (auto& leg : exits.legs) leg.side = leg.side == Side::Buy ? Side::Sell : Side::Buy;
  exits.type = OrderType::Limit;
  exits.tif = TimeInForce::Gtc;
  exits.limit_price = m("-6");
  exits.exits_only = true;
  exits.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Combo, TriggerDirection::AtOrAbove, m("1")}, {}}, ExitSpec{{}, m("-6")}};
  const auto before = s.snapshot()->buying_power.available;
  const auto p = s.preview(exits, f.time);
  ASSERT_TRUE(p.decision.ok()) << p.decision.message;
  ASSERT_TRUE(p.buying_power_after);
  EXPECT_EQ(p.buying_power_before, before);
  EXPECT_EQ(p.buying_power_required, m("2.60"));
  EXPECT_EQ(*p.buying_power_after, before - m("2.60"));
  EXPECT_FALSE(p.max_units);
  ASSERT_TRUE(s.submit(exits, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.available, *p.buying_power_after);
}
TEST(TradingPreview, ExecutionShowsWhatFillsAtOnceAndWhatHappensToTheRest) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s, "4", "4.20", 15);
  // As displayed: 15 of 20 fill at the ask and the other 5 cancel.
  const auto p = s.preview(f.market("twenty", 20), f.time);
  ASSERT_TRUE(p.decision.ok());
  EXPECT_EQ(p.execution.status, OrderStatus::Cancelled);
  EXPECT_EQ(p.execution.filled_quantity, 15);
  EXPECT_EQ(p.execution.remaining_quantity, 5);
  EXPECT_EQ(p.execution.reason.code, Reason::IOC_REMAINDER);
  ASSERT_EQ(p.execution.fills.size(), 1U);
  EXPECT_EQ(p.execution.fills[0].quantity, 15);
  EXPECT_EQ(p.execution.fills[0].price, m("4.20"));
  EXPECT_EQ(p.execution.average_fill_price, m("4.20"));
  // The full-size projection still prices all 20 at the far side.
  ASSERT_EQ(p.execution.schedule.size(), 1U);
  EXPECT_EQ(p.execution.schedule[0].quantity, 20);
  EXPECT_EQ(p.execution.average_price, m("4.20"));
  // Nothing was taken: submitting gets the same fills.
  ASSERT_TRUE(s.submit(f.market("twenty", 20), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders.back().filled_quantity, 15);
  // A limit below the ask rests.
  const auto resting = s.preview(f.limit("rest", 2, "3.00"), f.time);
  EXPECT_EQ(resting.execution.status, OrderStatus::Working);
  EXPECT_EQ(resting.execution.filled_quantity, 0);
  EXPECT_EQ(resting.execution.remaining_quantity, 2);
  EXPECT_TRUE(resting.execution.fills.empty());
  // A refused order executes nothing.
  const auto refused = s.preview(f.limit("tick", 1, "4.01"), f.time);
  EXPECT_EQ(refused.execution.status, OrderStatus::Rejected);
  EXPECT_EQ(refused.execution.reason.code, Reason::INVALID_TICK);
}
TEST(TradingPreview, ExecutionWalksImpactBlocksAndWaitsForFillLatency) {
  // An ask of 1.05 for 8 contracts; each further block of 8 costs one tick (0.05) more.
  ScriptedMarket f; auto c = config();
  c.initial_cash = m("100000"); c.rules.max_drawdown = m("6000"); c.rules.impact_ticks = 1;
  TradingSession s(c, f.time); f.seed(s, "1.00", "1.05", 8);
  const auto p = s.preview(f.market("26", 26), f.time);
  ASSERT_EQ(p.execution.schedule.size(), 4U);
  const std::vector<std::pair<Quantity, Money>> blocks{{8, m("1.05")}, {8, m("1.10")}, {8, m("1.15")}, {2, m("1.20")}};
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    EXPECT_EQ(p.execution.schedule[i].quantity, blocks[i].first);
    EXPECT_EQ(p.execution.schedule[i].price, blocks[i].second);
  }
  // 2,880 of premium over 26 contracts.
  EXPECT_EQ(p.execution.average_price, m("1.107692"));
  EXPECT_EQ(p.execution.status, OrderStatus::Filled);
  EXPECT_EQ(p.execution.filled_quantity, 26);
  EXPECT_EQ(p.execution.fills.size(), 4U);
  EXPECT_EQ(p.execution.average_fill_price, m("1.107692"));
  // With fill latency nothing fills on acceptance: the order works until a later quote.
  c.rules.impact_ticks = 0; c.rules.fill_latency_ms = 1000;
  TradingSession delayed(c, f.time); f.seed(delayed, "1.00", "1.05", 8);
  const auto waiting = delayed.preview(f.market("delayed", 2), f.time);
  EXPECT_EQ(waiting.execution.status, OrderStatus::Working);
  EXPECT_EQ(waiting.execution.filled_quantity, 0);
}
TEST(TradingPreview, ASpreadsScheduleNetsItsLegs) {
  ScriptedMarket f, wing; wing.contract.strike = 5010;
  TradingSession s(config(), f.time); f.seed(s); wing.seed(s, "2", "2.20");
  const auto p = s.preview(spread(f, wing, "spread", 2), f.time);
  ASSERT_EQ(p.execution.schedule.size(), 2U);
  EXPECT_EQ(p.execution.schedule[0].side, Side::Buy);
  EXPECT_EQ(p.execution.schedule[1].side, Side::Sell);
  // Buy at 4.20, sell at 2.00: a net debit of 2.20 a unit.
  EXPECT_EQ(p.execution.average_price, m("2.20"));
  EXPECT_EQ(p.execution.average_fill_price, m("2.20"));
  EXPECT_EQ(p.execution.filled_quantity, 2);
}
}  // namespace
}  // namespace openport::trading

#include <gtest/gtest.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
Timestamp at(md::Date date, int hour, int minute) { return md::new_york_to_utc(date, hour, minute); }
constexpr md::Date kTuesday{2026, 9, 22};
constexpr md::Date kWednesday{2026, 9, 23};
SessionConfig roomy(AccountRules rules = {}) {
  SessionConfig c;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
/// A new quote and valuation for the contract at `time`.
void tick(TradingSession& s, ScriptedMarket& f, Timestamp time, std::string_view bid = "4.00", std::string_view ask = "4.20") {
  f.time = time;
  ++f.observation;
  s.on_quotes({f.quote(bid, ask)}, {f.valuation()}, f.time);
}
const Order& order(const TradingSession& s, OrderId id) { return s.snapshot()->recent_orders.at(static_cast<std::size_t>(id - 1)); }

TEST(TradingSessions, TheOvernightSessionTakesLimitOrdersThatLastUntilItEnds) {
  ScriptedMarket f;
  f.time = at(kTuesday, 21, 0);  // Wednesday's overnight session
  TradingSession s(roomy(), f.time);
  f.seed(s);
  const auto night = s.submit(f.limit("night", 1, "4.10"), f.time);
  ASSERT_TRUE(night.decision.ok()) << night.decision.message;
  EXPECT_EQ(order(s, 1).status, OrderStatus::Working);
  EXPECT_EQ(order(s, 1).day_end, at(kWednesday, 9, 25));
  tick(s, f, at(kWednesday, 1, 0), "4.00", "4.10");
  ASSERT_EQ(s.snapshot()->recent_fills.size(), 1);
  EXPECT_EQ(s.snapshot()->recent_fills[0].price, m("4.10"));
  ASSERT_TRUE(s.submit(f.limit("unfilled", 1, "3.50"), f.time).decision.ok());
  tick(s, f, at(kWednesday, 9, 25));
  EXPECT_EQ(order(s, 2).status, OrderStatus::Cancelled);
  EXPECT_EQ(order(s, 2).reason.code, Reason::DAY_END);
  // A regular-session DAY order still ends with the regular session.
  tick(s, f, at(kWednesday, 9, 30));
  ASSERT_TRUE(s.submit(f.limit("regular", 1, "3.50"), f.time).decision.ok());
  EXPECT_EQ(order(s, 3).day_end, at(kWednesday, 16, 15));
}

TEST(TradingSessions, OutsideTheRegularSessionOnlyPlainLimitOrdersAreTaken) {
  ScriptedMarket f;
  f.time = at(kTuesday, 16, 30);  // curb
  TradingSession s(roomy(), f.time);
  f.seed(s);
  EXPECT_EQ(s.submit(f.market("market"), f.time).decision.code, Reason::LIMIT_ONLY);
  auto triggered = f.limit("triggered", 1, "4.10");
  triggered.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("4.00")};
  EXPECT_EQ(s.submit(triggered, f.time).decision.code, Reason::LIMIT_ONLY);
  auto bracket = f.limit("bracket", 1, "4.20");
  bracket.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.00")}, {}}, {}};
  EXPECT_EQ(s.submit(bracket, f.time).decision.code, Reason::LIMIT_ONLY);
  ASSERT_TRUE(s.submit(f.limit("curb", 1, "4.00"), f.time).decision.ok());
  EXPECT_EQ(order(s, 4).day_end, at(kTuesday, 17, 0));
  // Between sessions nothing trades, and the curb order has ended.
  tick(s, f, at(kTuesday, 18, 0));
  EXPECT_EQ(order(s, 4).reason.code, Reason::DAY_END);
  EXPECT_EQ(s.submit(f.limit("evening", 1, "4.00"), f.time).decision.code, Reason::SESSION_CLOSED);
  // Flattening sends market orders, so it waits for the regular session too.
  tick(s, f, at(kTuesday, 21, 0));
  ASSERT_TRUE(s.submit(f.limit("long", 1, "4.20"), f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->positions.size(), 1);
  ASSERT_TRUE(s.close_positions(std::nullopt, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders.back().reason.code, Reason::LIMIT_ONLY);
  EXPECT_EQ(s.snapshot()->positions.size(), 1);
}

TEST(TradingSessions, MultiLegOrdersTakeANetLimitOvernight) {
  const auto call = *md::parse_osi("SPXW261022C05100000");
  const auto put = *md::parse_osi("SPXW261022P04900000");
  const auto time = at(kTuesday, 22, 0);
  TradingSession s(roomy(), time);
  std::vector<QuoteObservation> quotes;
  std::vector<Valuation> valuations;
  for (const auto& c : {call, put}) {
    ASSERT_TRUE(s.define(c, time).decision.ok());
    quotes.push_back({c.osi_symbol(), 1, time, m("4.00"), m("4.20"), 10, 10});
    valuations.push_back({c.osi_symbol(), time, 0.3, 0.001, 2.0, -0.1, 5000, 5010, 0.99,
                          md::years_between(time, c.expiry_time()), 0.20, true});
  }
  s.on_quotes(quotes, valuations, time);
  OrderRequest strangle;
  strangle.client_order_id = "strangle";
  strangle.type = OrderType::Market;
  strangle.tif = TimeInForce::Ioc;
  strangle.quantity = 1;
  strangle.legs = {{call.osi_symbol(), Side::Buy, 1}, {put.osi_symbol(), Side::Buy, 1}};
  EXPECT_EQ(s.submit(strangle, time).decision.code, Reason::LIMIT_ONLY);
  strangle.client_order_id = "strangle-limit";
  strangle.type = OrderType::Limit;
  strangle.tif = TimeInForce::Day;
  strangle.limit_price = m("8.40");
  const auto placed = s.submit(strangle, time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  EXPECT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Filled);
}

TEST(TradingSessions, OptionsWithoutExtendedSessionsTradeRegularHoursOnly) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPY261022C00500000");
  f.time = at(kTuesday, 16, 30);
  TradingSession s(roomy(), f.time);
  f.seed(s);
  EXPECT_EQ(s.submit(f.limit("after-close", 1, "4.10"), f.time).decision.code, Reason::SESSION_CLOSED);
  tick(s, f, at(kTuesday, 21, 0));
  EXPECT_EQ(s.submit(f.limit("night", 1, "4.10"), f.time).decision.code, Reason::SESSION_CLOSED);
}

TEST(TradingSessions, AmSettledSeriesStopAtTheRegularCloseBeforeExpiry) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPX261016C05000000");  // expires at Friday's open
  const md::Date thursday{2026, 10, 15};
  f.time = at(thursday, 9, 0);  // Thursday's overnight session still trades it
  TradingSession s(roomy(), f.time);
  f.seed(s);
  EXPECT_TRUE(s.submit(f.limit("thursday-morning", 1, "4.00"), f.time).decision.ok());
  tick(s, f, at(thursday, 16, 30));
  EXPECT_EQ(s.submit(f.limit("curb", 1, "4.00"), f.time).decision.code, Reason::SESSION_CLOSED);
  tick(s, f, at(thursday, 21, 0));
  EXPECT_EQ(s.submit(f.limit("night", 1, "4.00"), f.time).decision.code, Reason::SESSION_CLOSED);
  // The PM weekly of the same date trades that night.
  ScriptedMarket weekly;
  weekly.contract = *md::parse_osi("SPXW261016C05000000");
  weekly.time = f.time;
  weekly.seed(s);
  EXPECT_TRUE(s.submit(weekly.limit("weekly-night", 1, "4.00"), weekly.time).decision.ok());
}

TEST(TradingSessions, ExitsAndTriggeredOrdersWaitForTheRegularSession) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  auto entry = f.limit("entry", 1, "4.20");
  entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.50")}, {}}, {}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  auto dip = f.limit("dip", 1, "3.40");
  dip.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.40")};
  ASSERT_TRUE(s.submit(dip, f.time).decision.ok());
  ASSERT_EQ(order(s, 2).status, OrderStatus::Armed);  // the stop
  // Overnight the stop and the trigger are crossed; neither acts.
  tick(s, f, at(kTuesday, 21, 0), "2.90", "3.10");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 3).status, OrderStatus::Armed);
  EXPECT_EQ(s.snapshot()->positions.size(), 1);
  // Plain overnight limit orders still trade beside them.
  ASSERT_TRUE(s.submit(f.limit("overnight", 1, "3.10"), f.time).decision.ok());
  EXPECT_EQ(order(s, 4).status, OrderStatus::Filled);
  tick(s, f, at(kWednesday, 9, 31), "2.90", "3.10");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 3).status, OrderStatus::Filled);
}

TEST(TradingSessions, AnOvernightTradeBelongsToTheNextTradingDate) {
  ScriptedMarket f;
  AccountRules rules;
  rules.plan = "Overnight";
  rules.profit_target = m("10000");
  rules.max_drawdown = m("5000");
  TradingSession s(roomy(rules), f.time);
  f.seed(s);
  EXPECT_EQ(s.trading_day(), kTuesday);
  tick(s, f, at(kTuesday, 16, 59));
  EXPECT_EQ(s.roll_day(at(kTuesday, 16, 59)).decision.code, Reason::INVALID_TIME);
  // The day ends with the curb session: Tuesday closes and the evening is Wednesday's.
  ASSERT_TRUE(s.roll_day(at(kTuesday, 17, 0)).decision.ok());
  EXPECT_EQ(s.trading_day(), kWednesday);
  ASSERT_FALSE(s.snapshot()->evaluation.days.empty());
  EXPECT_EQ(s.snapshot()->evaluation.days.back().day, kTuesday);
  tick(s, f, at(kTuesday, 21, 0));
  ASSERT_TRUE(s.submit(f.limit("night", 1, "4.20"), f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->recent_fills.size(), 1);
  EXPECT_EQ(s.snapshot()->evaluation.day, kWednesday);
  tick(s, f, at(kWednesday, 9, 0), "3.00", "3.20");
  EXPECT_EQ(s.trading_day(), kWednesday);  // no second rollover overnight
  EXPECT_EQ(s.roll_day(at(kWednesday, 10, 0)).decision.code, Reason::INVALID_TIME);
}

TEST(TradingSessions, AClosedMarketsCloseKeepsItsPositionsMarked) {
  // An SPY position held overnight keeps its closing mark, so SPX trades on.
  ScriptedMarket spy;
  spy.contract = *md::parse_osi("SPY261022C00500000");
  spy.time = at(kTuesday, 16, 14);
  TradingSession s(roomy(), spy.time);
  spy.seed(s);
  ASSERT_TRUE(s.submit(spy.limit("spy", 1, "4.20"), spy.time).decision.ok());
  tick(s, spy, at(kTuesday, 16, 15));  // the close
  ScriptedMarket spx;
  spx.time = at(kTuesday, 21, 0);
  spx.seed(s);
  EXPECT_TRUE(s.snapshot()->valuation_complete);
  EXPECT_TRUE(s.snapshot()->risk.complete);
  EXPECT_TRUE(s.submit(spx.limit("overnight", 1, "4.20"), spx.time).decision.ok());
  EXPECT_TRUE(s.roll_day(spx.time).decision.ok());
  // A feed that stops while its market is open is still stale.
  s.on_quotes({}, {}, at(kTuesday, 21, 5));
  EXPECT_FALSE(s.snapshot()->valuation_complete);
  EXPECT_EQ(s.submit(spx.limit("stale", 1, "4.20"), at(kTuesday, 21, 5)).decision.code, Reason::STALE_QUOTE);
}

}  // namespace
}  // namespace openport::trading

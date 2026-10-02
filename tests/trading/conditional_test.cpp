#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig roomy(AccountRules rules = {}) {
  SessionConfig c;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
Trigger trigger(TriggerSource source, TriggerDirection direction, std::string_view level) {
  return {source, direction, m(level)};
}
OrderRequest with_bracket(OrderRequest request, std::optional<ExitSpec> stop, std::optional<ExitSpec> target) {
  request.bracket = Bracket{std::move(stop), std::move(target)};
  return request;
}
ExitSpec stop_at(std::string_view bid) { return {trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, bid), {}}; }
ExitSpec target_at(std::string_view limit) { return {{}, m(limit)}; }
void quote(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask, Quantity size = 10,
           double spot = 5000) {
  f.next();
  auto valuation = f.valuation();
  valuation.spot = spot;
  s.on_quotes({f.quote(bid, ask, size)}, {valuation}, f.time);
}
const Order& order(const TradingSession& s, OrderId id) { return s.snapshot()->recent_orders.at(static_cast<std::size_t>(id - 1)); }

TEST(TradingConditional, BracketExitsSlipAndTakeProfitKeepsItsLimit) {
  for (const bool stop : {false, true}) {
    ScriptedMarket f;
    AccountRules rules;
    rules.slippage_ticks = 2;
    TradingSession s(roomy(rules), f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(with_bracket(f.market("entry"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
    quote(s, f, stop ? "3.50" : "5.10", stop ? "3.70" : "5.30");
    const auto snap = s.snapshot();
    ASSERT_EQ(snap->recent_fills.size(), 2U);
    EXPECT_EQ(snap->recent_fills.front().price, m("4.40"));
    EXPECT_EQ(snap->recent_fills.back().price, m(stop ? "3.30" : "5.00"));
    EXPECT_TRUE(snap->positions.empty());
    EXPECT_EQ(snap->account.fees, m("1.30"));
  }
}

TEST(TradingConditional, ArmedEntryActivatesWhenTheUnderlyingCrossesAndReservesUntilThen) {
  ScriptedMarket f;
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(roomy(rules), f.time);
  f.seed(s);
  auto request = f.market("breakout", 2);
  request.trigger = trigger(TriggerSource::Underlying, TriggerDirection::AtOrAbove, "5010");
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  EXPECT_EQ(order(s, 1).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 1).day_end, f.contract.last_trade_time());  // good until expiry
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("841.30"));  // 2 * 100 * ask 4.20 + fees
  EXPECT_EQ(s.snapshot()->open_orders.size(), 1);
  quote(s, f, "4.00", "4.20", 10, 5009.99);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Armed);
  quote(s, f, "4.50", "4.70", 10, 5010);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 1).triggered_at, f.time);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("4.70"));
}

TEST(TradingConditional, ReachedLevelsActivateAtSubmissionAndOptionTriggersUseTheExecutableSide) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  auto request = f.limit("now", 1, "4.20");
  request.trigger = trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "4.20");  // buys compare the ask
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  auto bad = f.limit("bad", 1, "4.20");
  bad.trigger = trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "0");
  EXPECT_EQ(s.submit(bad, f.time).decision.code, Reason::INVALID_ORDER);
}

TEST(TradingConditional, BracketStopTriggersOnTheBidAndCancelsTheTakeProfit) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 2, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  const auto& entry = order(s, 1);
  EXPECT_EQ(entry.status, OrderStatus::Filled);
  EXPECT_EQ(entry.stop_loss, 2);
  EXPECT_EQ(entry.take_profit, 3);
  const auto& stop = order(s, 2);
  EXPECT_EQ(stop.status, OrderStatus::Armed);
  EXPECT_EQ(stop.role, OrderRole::StopLoss);
  EXPECT_EQ(stop.request.side, Side::Sell);
  EXPECT_EQ(stop.request.type, OrderType::Market);
  EXPECT_EQ(stop.request.quantity, 2);
  EXPECT_EQ(stop.request.client_order_id, "entry:stop");
  EXPECT_EQ(stop.parent, 1);
  EXPECT_EQ(stop.oco, 3);
  const auto& target = order(s, 3);
  EXPECT_EQ(target.status, OrderStatus::Working);
  EXPECT_EQ(target.request.limit_price, m("5.00"));
  EXPECT_EQ(target.oco, 2);
  EXPECT_EQ(target.day_end, f.contract.expiry_time());
  quote(s, f, "3.60", "3.80");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  // A gap below the stop still executes: exits skip the price band.
  quote(s, f, "2.00", "3.60");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("2.00"));
  EXPECT_EQ(order(s, 3).status, OrderStatus::Cancelled);
  EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, BracketTakeProfitFillsAndCancelsTheStop) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  s.submit(with_bracket(f.limit("entry", 2, "4.20"), stop_at("3.50"), target_at("5.00")), f.time);
  quote(s, f, "5.00", "5.20");
  EXPECT_EQ(order(s, 3).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("5.00"));
  EXPECT_EQ(order(s, 2).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, APartialStopFillReArmsForTheRestAndKeepsTheTargetForIt) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 3, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  // The bid that reaches the stop shows one contract: the stop sells it and re-arms for the other two.
  quote(s, f, "3.40", "3.60", 1);
  EXPECT_EQ(order(s, 2).filled_quantity, 1);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 2).remaining(), 2);
  // The target is not cancelled: it shrinks to the two contracts still held.
  EXPECT_EQ(order(s, 3).status, OrderStatus::Working);
  EXPECT_EQ(order(s, 3).request.quantity, 2);
  // The next quote still at the stop sells the rest; only then is the target cancelled.
  quote(s, f, "3.30", "3.50", 5);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("3.30"));
  EXPECT_EQ(s.snapshot()->recent_fills.back().quantity, 2);
  EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, AStopThatFindsTheBidUsedUpReArmsInsteadOfCancelling) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(with_bracket(f.limit("first", 1, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  ASSERT_TRUE(s.submit(with_bracket(f.limit("second", 1, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  // Both stops are reached on a bid of one contract: the first takes it, and the
  // second, finding nothing left, re-arms rather than cancel and leave its contract bare.
  quote(s, f, "3.40", "3.60", 1);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 5).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 5).filled_quantity, 0);
  EXPECT_TRUE(order(s, 6).open());
  quote(s, f, "3.40", "3.60", 1);
  EXPECT_EQ(order(s, 5).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 6).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, APartialTargetFillShrinksTheStopInsteadOfCancellingIt) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 3, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  quote(s, f, "5.00", "5.20", 1);
  EXPECT_EQ(order(s, 3).status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(order(s, 3).filled_quantity, 1);
  // The stop still protects the two contracts left.
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 2).request.quantity, 2);
  quote(s, f, "3.40", "3.60", 5);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().quantity, 2);
  EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, APartlyFilledExitCancelsItsSiblingWithoutClosingAnotherEntry) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 3, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("another entry"), f.time).decision.ok());
  quote(s, f, "3.40", "3.60", 1);
  EXPECT_EQ(order(s, 2).filled_quantity, 1);
  EXPECT_EQ(order(s, 3).remaining(), 2);
  // The stop's last two contracts finish this bracket; the other entry stays held.
  quote(s, f, "3.40", "3.60", 2);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
  ASSERT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_EQ(s.snapshot()->positions.front().position.quantity, 1);
}

TEST(TradingConditional, AStopLimitExitRestsAtItsLimitOnceReachedAndWaitsThroughAGap) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ExitSpec stop_limit{trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "3.50"), m("3.40")};
  ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 2, "4.20"), stop_limit, target_at("5.00")), f.time).decision.ok());
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 2).request.type, OrderType::Limit);
  EXPECT_EQ(order(s, 2).request.tif, TimeInForce::Gtc);
  // The bid gaps through the stop and its limit: the stop triggers and rests at 3.40, far
  // below the band around the mid, rather than sell at 3.00 or cancel.
  quote(s, f, "3.00", "3.20");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Working);
  EXPECT_EQ(order(s, 2).triggered_at, f.time);
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 1U);
  EXPECT_TRUE(order(s, 3).open());
  // Back at its limit, it sells, and the target is cancelled.
  quote(s, f, "3.40", "3.60");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("3.40"));
  EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  // Its limit, like a take-profit's, takes the product tick.
  ExitSpec off_tick{trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "3.50"), m("3.42")};
  EXPECT_EQ(s.submit(with_bracket(f.limit("tick", 1, "3.60"), off_tick, {}), f.time).decision.code, Reason::INVALID_TICK);
}

TEST(TradingConditional, AStopLimitOrderIsBandedAroundItsStopAndKeepsWorkingThroughAGap) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("long", 1, "4.20"), f.time).decision.ok());
  // A protective stop-limit well below the market: its limit is 1.20 from the mid
  // (beyond the 0.82 band there) but 0.10 from its stop, which is what it is set against.
  auto protect = f.limit("protect", 1, "2.90", Side::Sell, TimeInForce::Gtc);
  protect.trigger = trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "3.00");
  ASSERT_TRUE(s.submit(protect, f.time).decision.ok());
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  // A limit far from its own stop is still refused.
  auto far = f.limit("far", 1, "1.00", Side::Sell, TimeInForce::Gtc);
  far.trigger = trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "3.00");
  EXPECT_EQ(s.submit(far, f.time).decision.code, Reason::PRICE_BAND);
  // The market gaps to 2.10: the order triggers and keeps working at 2.90 instead of
  // being cancelled for a limit outside the band around the new mid.
  quote(s, f, "2.00", "2.20");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Working);
  EXPECT_EQ(order(s, 2).triggered_at, f.time);
  quote(s, f, "2.90", "3.10");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("2.90"));
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, ExitsGrowWithEntryFillsAndShrinkOrCancelWithManualCloses) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s, "4.00", "4.20", 2);
  s.submit(with_bracket(f.limit("entry", 5, "4.20"), stop_at("3.50"), target_at("5.00")), f.time);
  EXPECT_EQ(order(s, 1).filled_quantity, 2);
  EXPECT_EQ(order(s, 2).request.quantity, 2);
  EXPECT_EQ(order(s, 3).request.quantity, 2);
  quote(s, f, "4.00", "4.20", 3);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 2).request.quantity, 5);
  EXPECT_EQ(order(s, 3).request.quantity, 5);
  s.submit(f.market("trim", 3, Side::Sell), f.time);
  EXPECT_EQ(order(s, 2).request.quantity, 2);
  EXPECT_EQ(order(s, 3).request.quantity, 2);
  quote(s, f, "4.00", "4.20");
  s.submit(f.market("flat", 2, Side::Sell), f.time);
  EXPECT_EQ(order(s, 2).reason.code, Reason::POSITION_CLOSED);
  EXPECT_EQ(order(s, 3).reason.code, Reason::POSITION_CLOSED);
  EXPECT_TRUE(s.snapshot()->open_orders.empty());
}

TEST(TradingConditional, BuyOnlyAndBuyingPowerCountTheBracketOnceAndNeverBlockManualCloses) {
  ScriptedMarket f;
  AccountRules rules;
  rules.buy_only = true;
  rules.buying_power = true;
  TradingSession s(roomy(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 2, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  // Two exits for 2 contracts reserve only one set of fees.
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("1.30"));
  // A sell stop protecting a long is a closing order, even on a buy-only plan.
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.submit(f.limit("manual", 2, "4.40", Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.submit(f.limit("extra", 1, "4.40", Side::Sell), f.time).decision.code, Reason::BUY_ONLY);
}

TEST(TradingConditional, AnOffTickExitPriceIsAnInvalidTick) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  // A take-profit, and a stop exit that rests as a limit, off the contract's tier tick.
  EXPECT_EQ(s.submit(with_bracket(f.limit("target", 1, "4.10"), stop_at("4.00"), target_at("5.03")), f.time).decision.code,
            Reason::INVALID_TICK);
  EXPECT_EQ(s.submit(with_bracket(f.limit("stop", 1, "4.10"), target_at("4.03"), target_at("5.00")), f.time).decision.code,
            Reason::INVALID_TICK);
  EXPECT_TRUE(s.submit(with_bracket(f.limit("on-tick", 1, "4.10"), target_at("4.00"), target_at("5.00")), f.time).decision.ok());
}

TEST(TradingConditional, ABracketStopAlreadyReachedWhenTheEntryFillsFiresAtSubmission) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  // The bid, 4.00, is already at the stop: the stop sells at once, at that bid.
  ASSERT_TRUE(s.submit(with_bracket(f.market("entry", 2), stop_at("4.10"), target_at("5.00")), f.time).decision.ok());
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 2).triggered_at, f.time);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("4.00"));
  EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  // So does one whose entry a change makes marketable, and one a triggered entry's fill creates.
  ASSERT_TRUE(s.submit(with_bracket(f.limit("changed", 1, "4.10"), stop_at("4.00"), target_at("5.00")), f.time).decision.ok());
  EXPECT_EQ(order(s, 4).status, OrderStatus::Working);
  ASSERT_TRUE(s.modify(4, {{}, m("4.20"), {}}, f.time).decision.ok());
  EXPECT_EQ(order(s, 5).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 6).reason.code, Reason::OCO_FILLED);
  auto armed = with_bracket(f.market("armed"), stop_at("4.00"), target_at("5.00"));
  armed.trigger = trigger(TriggerSource::Underlying, TriggerDirection::AtOrAbove, "4990");
  ASSERT_TRUE(s.submit(armed, f.time).decision.ok());
  EXPECT_EQ(order(s, 7).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 8).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 9).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, ATriggeredDayOrderLastsTheSessionItActivatedIn) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  auto request = f.limit("dip", 1, "3.60");
  request.trigger = trigger(TriggerSource::Underlying, TriggerDirection::AtOrBelow, "4990");
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  // Armed, it lasts until the contract's last trade.
  EXPECT_EQ(order(s, 1).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 1).day_end, f.contract.last_trade_time());
  // The next day it activates, and then lasts that session, like any DAY order.
  f.time = md::new_york_to_utc({2026, 9, 23}, 11, 0);
  quote(s, f, "4.00", "4.20", 10, 4989);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Working);
  const auto end = md::new_york_to_utc({2026, 9, 23}, 16, 15);
  EXPECT_EQ(order(s, 1).day_end, end);
  s.on_quotes({}, {}, end);
  EXPECT_EQ(order(s, 1).reason.code, Reason::DAY_END);
  // A GTC one keeps waiting past the session end.
  f.time = md::new_york_to_utc({2026, 9, 24}, 11, 0);
  quote(s, f, "4.00", "4.20", 10, 5000);
  auto gtc = f.limit("gtc-dip", 1, "3.60", Side::Buy, TimeInForce::Gtc);
  gtc.trigger = trigger(TriggerSource::Underlying, TriggerDirection::AtOrBelow, "5000");
  ASSERT_TRUE(s.submit(gtc, f.time).decision.ok());
  EXPECT_EQ(order(s, 2).status, OrderStatus::Working);
  EXPECT_EQ(order(s, 2).day_end, f.contract.last_trade_time());
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 9, 24}, 16, 15));
  EXPECT_EQ(order(s, 2).status, OrderStatus::Working);
}

TEST(TradingConditional, ArmedOrdersAndExitsReportTheLastTradeOrAutoCloseAsTheirDeadline) {
  for (const bool cutoff : {false, true}) {
    // An AM-settled monthly stops trading at the close the day before it expires.
    ScriptedMarket f;
    f.contract = *md::parse_osi("SPX261016C05000000");
    auto rules = AccountRules{};
    if (cutoff) rules.expiry_cutoff = 5 * md::kNanosPerMinute;
    TradingSession s(roomy(rules), f.time);
    f.seed(s);
    const auto deadline = md::new_york_to_utc({2026, 10, 15}, 16, cutoff ? 10 : 15);
    ASSERT_EQ(f.contract.last_trade_time() - rules.expiry_cutoff, deadline);
    auto armed = f.limit("armed", 1, "3.60");
    armed.trigger = trigger(TriggerSource::Underlying, TriggerDirection::AtOrBelow, "4900");
    ASSERT_TRUE(s.submit(armed, f.time).decision.ok());
    EXPECT_EQ(order(s, 1).day_end, deadline);
    ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 1, "4.20"), stop_at("3.00"), target_at("5.00")), f.time).decision.ok());
    // Both exits last until the deadline, and the take-profit says so: it is good until then.
    for (const OrderId id : {OrderId{3}, OrderId{4}}) EXPECT_EQ(order(s, id).day_end, deadline);
    EXPECT_EQ(order(s, 3).request.tif, TimeInForce::Ioc);
    EXPECT_EQ(order(s, 4).request.tif, TimeInForce::Gtc);
    s.on_quotes({}, {}, deadline);
    for (const OrderId id : {OrderId{1}, OrderId{3}, OrderId{4}})
      EXPECT_EQ(order(s, id).reason.code, cutoff ? Reason::EXPIRY_CUTOFF : Reason::EXPIRED) << id;
  }
}

TEST(TradingConditional, TriggersWaitForTheRegularSessionAndSurviveRecovery) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-conditional-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = (directory / "journal.jsonl").string();
  ScriptedMarket f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(roomy(), f.time, journal);
    f.seed(s);
    s.submit(with_bracket(f.limit("entry", 1, "4.20"), stop_at("3.50"), target_at("5.00")), f.time);
    // After the close a crossed stop waits for the regular session.
    f.time = md::new_york_to_utc({2026, 9, 22}, 16, 30);
    ++f.observation;
    s.on_quotes({f.quote("3.00", "3.20")}, {f.valuation()}, f.time);
    EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto recovered = TradingSession::recover(FileJournal::read(path, head), FileJournal::resume(path));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  f.time = md::new_york_to_utc({2026, 9, 23}, 9, 31);
  ++f.observation;
  recovered.on_quotes({f.quote("3.00", "3.20")}, {f.valuation()}, f.time);
  EXPECT_EQ(order(recovered, 2).status, OrderStatus::Filled);
  EXPECT_EQ(order(recovered, 3).reason.code, Reason::OCO_FILLED);
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace openport::trading

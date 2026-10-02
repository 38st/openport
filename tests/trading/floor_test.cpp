#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "support/scripted_market.hpp"
#include "trading/state.hpp"

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
void update(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask) {
  f.next();
  s.on_quotes({f.quote(bid, ask, 100)}, {f.valuation()}, f.time);
}
void roll(TradingSession& s, ScriptedMarket& f) {
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  ++f.observation;
  if (!s.contracts().contains(f.symbol())) s.define(f.contract, f.time);
  s.on_quotes({f.quote("4", "4.20", 100)}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
}
struct File {
  std::string path = (std::filesystem::temp_directory_path() / ("openport-floor-" + std::to_string(md::now()) + ".jsonl")).string();
  ~File() { std::error_code ignored; std::filesystem::remove(path, ignored); }
};

TEST(TradingFloor, EvaluationLimitsTightenNowLoosenAtRollIncludingMixedEdits) {
  ScriptedMarket f;
  TradingSession s(config(), f.time);
  f.seed(s);
  auto limits = s.config().limits;
  limits.max_order_contracts = 5;
  limits.max_daily_loss = m("100");
  s.set_limits(limits, f.time);
  EXPECT_EQ(s.config().limits.max_order_contracts, 5);
  EXPECT_FALSE(s.snapshot()->pending_limits);
  limits.max_order_contracts = 20;
  limits.max_daily_loss = m("50");
  limits.price_band_relative = 0.5;
  s.set_limits(limits, f.time);
  EXPECT_EQ(s.config().limits.max_order_contracts, 5);
  EXPECT_EQ(s.config().limits.max_daily_loss, m("50"));
  ASSERT_TRUE(s.snapshot()->pending_limits);
  EXPECT_EQ(s.snapshot()->pending_limits->max_order_contracts, 20);
  EXPECT_EQ(s.submit(f.market("too-big", 6), f.time).decision.code, Reason::MAX_ORDER_CONTRACTS);
  roll(s, f);
  EXPECT_EQ(s.config().limits.max_order_contracts, 20);
  EXPECT_DOUBLE_EQ(s.config().limits.price_band_relative, 0.5);
  EXPECT_FALSE(s.snapshot()->pending_limits);
  EXPECT_EQ(s.snapshot()->pending_applied_at, f.time);
}
TEST(TradingFloor, PracticeLimitsAreImmediateAndOverridesCannotBeLoosenedByRemoval) {
  ScriptedMarket f;
  TradingSession practice({}, f.time);
  auto limits = practice.config().limits;
  limits.max_order_contracts = 200;
  practice.set_limits(limits, f.time);
  EXPECT_EQ(practice.config().limits.max_order_contracts, 200);
  EXPECT_FALSE(practice.snapshot()->pending_limits);
  auto c = config();
  c.limits.underlying_overrides["SPX"] = {100, 50};
  TradingSession evaluation(c, f.time);
  auto changed = c.limits;
  changed.underlying_overrides.clear();
  changed.aggregate.dollar_delta = 2e9;
  evaluation.set_limits(changed, f.time);
  EXPECT_DOUBLE_EQ(evaluation.config().limits.underlying_overrides.at("SPX").dollar_delta, 100);
  ASSERT_TRUE(evaluation.snapshot()->pending_limits);
}
TEST(TradingFloor, AttemptResetAppliesPendingAndClearsGuardrailProgress) {
  ScriptedMarket f;
  auto c = config();
  c.guardrails.max_opening_trades = 1;
  TradingSession s(c, f.time);
  f.seed(s);
  s.submit(f.market("entry"), f.time);
  s.set_guardrails({}, f.time);
  auto limits = s.config().limits;
  limits.max_order_contracts = 200;
  s.set_limits(limits, f.time);
  s.reset_account(m("10000"), c.rules, "new attempt", f.time);
  EXPECT_EQ(s.snapshot()->evaluation.attempt, 2);
  EXPECT_EQ(s.config().limits.max_order_contracts, 200);
  EXPECT_EQ(s.config().guardrails.max_opening_trades, 0);
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 0);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
  EXPECT_FALSE(s.snapshot()->pending_guardrails);
}
TEST(TradingFloor, TradeLimitCountsOpeningExecutionsAndKeepsClosingOrdersAndFlatten) {
  ScriptedMarket f;
  auto c = config(); c.guardrails.max_opening_trades = 2;
  TradingSession s(c, f.time); f.seed(s);
  s.submit(f.market("first"), f.time);
  ASSERT_FALSE(s.snapshot()->risk.kill_latched);
  s.submit(f.limit("rest", 1, "3.50"), f.time);
  s.submit(f.market("second"), f.time);
  ASSERT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 2);
  EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Cancelled);
  EXPECT_EQ(s.submit(f.market("third"), f.time).decision.code, Reason::TRADE_LIMIT);
  EXPECT_EQ(s.reset_kill("bypass", f.time).decision.code, Reason::TRADE_LIMIT);
  ASSERT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.close_positions({}, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
  roll(s, f);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 0);
  EXPECT_TRUE(s.submit(f.market("next-day"), f.time).decision.ok());
}
TEST(TradingFloor, SoftFloorTouchFlattensAndLatchesWithoutFailingAttempt) {
  for (const bool percent : {false, true}) {
    ScriptedMarket f;
    auto c = config(); c.fee_per_contract = {};
    if (percent) c.guardrails.soft_floor_percent = 99; else c.guardrails.soft_floor = m("9990");
    TradingSession s(c, f.time); f.seed(s);
    s.submit(f.market("entry"), f.time);
    const auto snapshot = s.snapshot();
    EXPECT_TRUE(snapshot->positions.empty());
    EXPECT_EQ(snapshot->recent_fills.size(), 2U);
    EXPECT_EQ(snapshot->risk.kill_reason, "SOFT_FLOOR");
    EXPECT_EQ(snapshot->evaluation.status, EvaluationStatus::Active);
    EXPECT_EQ(s.submit(f.market("blocked"), f.time).decision.code, Reason::SOFT_FLOOR);
    EXPECT_EQ(s.reset_kill("cannot", f.time).decision.code, Reason::SOFT_FLOOR);
    roll(s, f);
    // The configured absolute soft floor is still above equity, so it trips again;
    // the percentage floor also follows the plan, not a freshly invented balance.
    EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  }
}
TEST(TradingFloor, SoftFloorRetriesUnfilledLiquidation) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  s.submit(f.market("entry", 3), f.time);
  Guardrails g; g.soft_floor = m("9990");
  f.next(); s.on_quotes({f.quote("4", "4.20", 1)}, {f.valuation()}, f.time);
  s.set_guardrails(g, f.time);
  ASSERT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_EQ(s.snapshot()->positions[0].position.quantity, 2);
  update(s, f, "4", "4.20");
  EXPECT_TRUE(s.snapshot()->positions.empty());
}
TEST(TradingFloor, ProfitLockLeavesPositionsAndExitsWorkingUntilNextDay) {
  ScriptedMarket f; auto c = config(); c.guardrails.profit_lock = m("20");
  TradingSession s(c, f.time); f.seed(s);
  auto order = f.market("entry");
  order.bracket = Bracket{{}, ExitSpec{{}, m("5")}};
  s.submit(order, f.time);
  update(s, f, "4.50", "4.70");
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "PROFIT_LOCK");
  EXPECT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_EQ(s.snapshot()->open_orders.size(), 1U);
  update(s, f, "5", "5.20");
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.submit(f.market("again"), f.time).decision.code, Reason::PROFIT_LOCK);
  roll(s, f);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
}
TEST(TradingFloor, GuardrailCancelsCarryTheCodeNewOrdersAreRefusedWith) {
  // B35: a latch cancelled working opening orders with KILL_SWITCH, a switch the trader
  // never used, while it refused new ones with its own code.
  for (const bool trade_limit : {false, true}) {
    ScriptedMarket f; auto c = config();
    if (trade_limit) c.guardrails.max_opening_trades = 1; else c.guardrails.profit_lock = m("20");
    TradingSession s(c, f.time); f.seed(s);
    ASSERT_TRUE(s.submit(f.limit("rest", 1, "3.50"), f.time).decision.ok());
    s.submit(f.market("entry"), f.time);
    if (!trade_limit) update(s, f, "4.50", "4.70");
    const auto cancelled = s.snapshot()->recent_orders[0];
    ASSERT_EQ(cancelled.status, OrderStatus::Cancelled);
    const auto refused = s.submit(f.market("again"), f.time).decision;
    EXPECT_EQ(refused.code, trade_limit ? Reason::TRADE_LIMIT : Reason::PROFIT_LOCK);
    EXPECT_EQ(cancelled.reason.code, refused.code);
    EXPECT_EQ(cancelled.reason.message, refused.message);
  }
  // The trader's own trip still cancels with KILL_SWITCH.
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("rest", 1, "3.50"), f.time).decision.ok());
  ASSERT_TRUE(s.trip_kill("stop for the day", f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::KILL_SWITCH);
}
TEST(TradingFloor, APlanDecisionLabelsItsLiquidationEvenWhenTheSoftFloorLatchesWithIt) {
  // D23: a soft floor that latched in the batch that failed the plan labelled the
  // plan's liquidation system:soft_floor:N.
  ScriptedMarket f; auto c = config(); c.guardrails.soft_floor = m("9500");
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("entry", 5), f.time).decision.ok());
  update(s, f, "2.00", "2.20");
  auto snapshot = s.snapshot();
  EXPECT_EQ(snapshot->evaluation.status, EvaluationStatus::Failed);
  EXPECT_EQ(snapshot->risk.kill_reason, "SOFT_FLOOR");
  EXPECT_TRUE(snapshot->recent_orders.back().request.client_order_id.starts_with("system:drawdown:"));
  EXPECT_TRUE(snapshot->positions.empty());
  // While the attempt is active, the soft floor's own label remains.
  ScriptedMarket g; auto soft = config(); soft.guardrails.soft_floor = m("9990");
  TradingSession t(soft, g.time); g.seed(t);
  t.submit(g.market("entry"), g.time);
  EXPECT_TRUE(t.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:soft_floor:"));
}
TEST(TradingFloor, CooldownAfterStopUsesOnlyMarketTimeAndAllowsCloses) {
  ScriptedMarket f; auto c = config(); c.guardrails.cooldown_minutes = 5;
  TradingSession s(c, f.time); f.seed(s);
  auto order = f.market("entry", 2);
  order.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.90")}, {}}, {}};
  s.submit(order, f.time);
  f.next(); s.on_quotes({f.quote("3.80", "4", 1)}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "COOLDOWN");
  EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, f.time + 5 * md::kNanosPerMinute);
  EXPECT_EQ(s.submit(f.market("blocked"), f.time).decision.code, Reason::COOLDOWN);
  EXPECT_EQ(s.reset_kill("blocked", f.time).decision.code, Reason::COOLDOWN);
  update(s, f, "4", "4.20");
  EXPECT_TRUE(s.close_positions({}, f.time).decision.ok());
  auto until = s.snapshot()->guardrails.cooldown_until;
  s.on_quotes({}, {}, until - 1);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  s.on_quotes({}, {}, until);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
}
TEST(TradingFloor, CooldownOnLosingCloseAndManualKillSurvivesExpiryAndRoll) {
  ScriptedMarket f; auto c = config(); c.guardrails.cooldown_minutes = 1; c.guardrails.cooldown_loss = m("10");
  TradingSession s(c, f.time); f.seed(s);
  s.submit(f.market("open"), f.time);
  s.submit(f.market("close", 1, Side::Sell), f.time);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "COOLDOWN");
  s.trip_kill("manual pause", f.time);
  s.on_quotes({}, {}, f.time + md::kNanosPerMinute);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "manual pause");
  roll(s, f);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_TRUE(s.reset_kill("ready", f.time).decision.ok());
}
TEST(TradingFloor, GuardrailsAreTightenOnlyEvenInPractice) {
  ScriptedMarket f; TradingSession s({}, f.time);
  Guardrails g; g.max_opening_trades = 10; g.cooldown_minutes = 5; g.cooldown_loss = m("100"); g.profit_lock = m("500");
  s.set_guardrails(g, f.time);
  g.max_opening_trades = 5; g.cooldown_loss = m("50"); g.cooldown_minutes = 10; g.profit_lock = m("200");
  s.set_guardrails(g, f.time);
  EXPECT_EQ(s.config().guardrails, g);
  EXPECT_FALSE(s.snapshot()->pending_guardrails);
  s.set_guardrails({}, f.time);
  EXPECT_EQ(s.config().guardrails, g);
  ASSERT_TRUE(s.snapshot()->pending_guardrails);
  roll(s, f);
  EXPECT_EQ(s.config().guardrails, Guardrails{});
}
TEST(TradingFloor, CooldownKeepsItsExpiryAcrossRollAndTighteningExtendsIt) {
  ScriptedMarket f; auto c = config();
  c.guardrails.cooldown_minutes = 1440; c.guardrails.cooldown_loss = m("10");
  TradingSession s(c, f.time); f.seed(s);
  s.submit(f.market("open"), f.time);
  s.submit(f.market("close", 1, Side::Sell), f.time);
  const auto until = s.snapshot()->guardrails.cooldown_until;
  s.on_quotes({}, {}, f.time + md::kNanosPerMinute);
  EXPECT_EQ(s.snapshot()->time, f.time + md::kNanosPerMinute);
  EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, until);
  const auto next_day = md::new_york_to_utc({2026, 9, 23}, 9, 30);
  s.roll_day(next_day);
  EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, until);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "COOLDOWN");
  EXPECT_EQ(s.submit(f.market("blocked"), next_day).decision.code, Reason::COOLDOWN);
  s.on_quotes({}, {}, until);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);

  // Start a separate, five-minute cooldown, then extend its original expiry.
  c.guardrails.cooldown_minutes = 5;
  TradingSession tightened(c, f.time); f.seed(tightened);
  tightened.submit(f.market("entry"), f.time);
  tightened.submit(f.market("exit", 1, Side::Sell), f.time);
  c.guardrails.cooldown_minutes = 10;
  tightened.set_guardrails(c.guardrails, f.time + md::kNanosPerMinute);
  EXPECT_EQ(tightened.snapshot()->guardrails.cooldown_until, f.time + 10 * md::kNanosPerMinute);
}
TEST(TradingFloor, DailyGuardrailKeepsTheSharedLatchAfterCooldownExpires) {
  ScriptedMarket f; auto c = config();
  c.guardrails.cooldown_minutes = 1; c.guardrails.cooldown_loss = m("10");
  TradingSession s(c, f.time); f.seed(s);
  s.submit(f.market("entry"), f.time); s.submit(f.market("exit", 1, Side::Sell), f.time);
  ASSERT_EQ(s.snapshot()->risk.kill_reason, "COOLDOWN");
  auto guardrails = c.guardrails; guardrails.max_opening_trades = 1;
  s.set_guardrails(guardrails, f.time);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "TRADE_LIMIT");
  s.on_quotes({}, {}, f.time + md::kNanosPerMinute);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "TRADE_LIMIT");
  EXPECT_EQ(s.reset_kill("cannot bypass", f.time + md::kNanosPerMinute).decision.code, Reason::TRADE_LIMIT);
}
TEST(TradingFloor, DailyExtremesAndClosestFloorRecordFirstTouchTimes) {
  ScriptedMarket f; auto c = config(); c.rules.drawdown_mode = DrawdownMode::EndOfDay;
  TradingSession s(c, f.time); f.seed(s);
  s.submit(f.market("open"), f.time);
  const auto low = s.snapshot()->equity;
  const auto low_at = f.time;
  update(s, f, "5", "5.20");
  const auto high = s.snapshot()->equity;
  const auto high_at = f.time;
  s.submit(f.market("close", 1, Side::Sell), f.time);
  roll(s, f);
  const auto& day = s.snapshot()->evaluation.days.front();
  EXPECT_EQ(day.low_equity, low);
  EXPECT_EQ(day.low_at, low_at);
  EXPECT_EQ(day.high_equity, high);
  EXPECT_EQ(day.high_at, high_at);
  EXPECT_EQ(s.snapshot()->evaluation.closest_floor, low - m("9000"));
  EXPECT_EQ(s.snapshot()->evaluation.closest_floor_at, low_at);
}
TEST(TradingFloor, PendingAndGuardrailStateSurviveJournalRecoveryAndOlderRecordsLoad) {
  File file; ScriptedMarket f;
  std::string expected;
  {
    auto c = config(); c.guardrails.max_opening_trades = 1;
    TradingSession s(c, f.time, FileJournal::create(file.path)); f.seed(s);
    s.submit(f.market("open"), f.time);
    s.set_guardrails({}, f.time);
    auto limits = s.config().limits; limits.max_order_contracts = 200; s.set_limits(limits, f.time);
    expected = s.snapshot_json();
  }
  auto restored = TradingSession::recover(FileJournal::read(file.path));
  EXPECT_EQ(restored.snapshot_json(), expected);
  EXPECT_EQ(restored.submit(f.market("blocked"), f.time).decision.code, Reason::TRADE_LIMIT);
  // The historical fixture predates every field introduced by this feature.
  const auto old = TradingSession::recover(FileJournal::read(std::string(OPENPORT_TEST_DATA_DIR) + "/kill-before-reduce-only.jsonl"));
  EXPECT_EQ(old.config().guardrails, Guardrails{});
  EXPECT_FALSE(old.snapshot()->pending_limits);
  EXPECT_FALSE(old.snapshot()->pending_guardrails);
  EXPECT_FALSE(old.snapshot()->evaluation.day_low_equity);
  EXPECT_FALSE(old.snapshot()->evaluation.closest_floor);
  EXPECT_EQ(old.snapshot()->guardrails.opening_trades, 0);
  EvaluationDay day = Json{{"day", md::Date{2026, 9, 22}}, {"open_equity", 1000}, {"close_equity", 1010}, {"peak", 1010}, {"floor", 900}}.get<EvaluationDay>();
  EXPECT_FALSE(day.low_equity);
  EXPECT_EQ(day.high_at, 0);
}
TEST(TradingPreview, PureChecksMatchSubmitAndDoNotConsumeIdsJournalOrLiquidity) {
  File file; ScriptedMarket f;
  auto sink = FileJournal::create(file.path);
  TradingSession s(config(), f.time, sink); f.seed(s, "4", "4.20", 1);
  const auto snapshot = s.snapshot_json(), head = sink->head();
  const auto sequence = sink->sequence();
  auto order = f.market("entry");
  const auto p = s.preview(order, f.time);
  EXPECT_TRUE(p.decision.ok());
  EXPECT_EQ(p.max_loss, m("420.65"));
  EXPECT_EQ(p.max_loss_basis, "expiry_payoff");
  EXPECT_EQ(p.max_units, 1);  // half of $1000 room, independently of displayed size
  EXPECT_EQ(p.buying_power_after, m("9579.35"));
  EXPECT_EQ(p.exposure_change->dollar_delta, 250000);
  EXPECT_EQ(s.snapshot_json(), snapshot);
  EXPECT_EQ(sink->head(), head);
  EXPECT_EQ(sink->sequence(), sequence);
  const auto submitted = s.submit(order, f.time);
  EXPECT_EQ(submitted.decision.code, p.decision.code);
  EXPECT_EQ(submitted.order_id, 1);
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 1U);
  const auto retry = s.preview(order, f.time - 1);
  EXPECT_EQ(retry.decision.code, submitted.decision.code);
  EXPECT_EQ(retry.buying_power_after, retry.buying_power_before);
  EXPECT_EQ(retry.buying_power_required, Money{});
  EXPECT_EQ(retry.exposure_change->dollar_delta, 0);
  EXPECT_FALSE(retry.max_units);
}
TEST(TradingPreview, SameRejectionsAndSizeHonoursPowerLimitsAndRoom) {
  ScriptedMarket f; auto c = config(); c.limits.max_order_contracts = 3;
  TradingSession s(c, f.time); f.seed(s, "4", "4.20", 1);
  for (int i = 0; i < 3; ++i) {
    auto request = f.market("reject-" + std::to_string(i), i == 0 ? 4 : 1);
    if (i == 1) request.symbol = "unknown";
    if (i == 2) s.trip_kill("pause", f.time);
    const auto preview = s.preview(request, f.time);
    EXPECT_EQ(preview.decision.code, s.submit(request, f.time).decision.code);
  }
  s.reset_kill("resume", f.time);
  EXPECT_EQ(s.preview(f.market("size", 1), f.time, 1).max_units, 2);
  EXPECT_THROW((void)s.preview(f.market("invalid"), f.time, 0), TradingError);
  c.rules.max_drawdown = {}; c.initial_cash = m("500");
  TradingSession small(c, f.time); f.seed(small);
  EXPECT_EQ(small.preview(f.market("small"), f.time).max_units, 1);
}
TEST(TradingPreview, ExactVerticalAndUnboundedScenarioLossAndFloorWarnings) {
  ScriptedMarket f, wing; wing.contract.strike = 5010;
  TradingSession s(config(), f.time); f.seed(s); wing.seed(s, "2", "2.20");
  OrderRequest spread = f.market("spread", 2);
  spread.symbol.clear(); spread.legs = {{f.symbol(), Side::Buy, 1}, {wing.symbol(), Side::Sell, 1}};
  const auto p = s.preview(spread, f.time);
  EXPECT_EQ(p.max_loss, m("442.60"));
  EXPECT_EQ(p.max_units, 2);
  EXPECT_FALSE(*p.breaches_floor);
  auto credit = spread; credit.client_order_id = "credit";
  credit.legs[0].side = Side::Sell; credit.legs[1].side = Side::Buy;
  EXPECT_EQ(s.preview(credit, f.time).max_loss, m("1642.60"));
  auto short_call = f.market("naked", 1, Side::Sell);
  const auto naked = s.preview(short_call, f.time);
  EXPECT_EQ(naked.max_loss_basis, "scenario_grid");
  ASSERT_TRUE(naked.max_loss);
  EXPECT_GT(*naked.max_loss, Money{});
  EXPECT_TRUE(*naked.breaches_floor);
  Guardrails g; g.soft_floor = m("9700"); s.set_guardrails(g, f.time);
  EXPECT_TRUE(*s.preview(f.market("long"), f.time).breaches_soft_floor);
  EXPECT_EQ(s.preview(f.market("long"), f.time).max_units, 0);
}
TEST(TradingPreview, SizeCanHedgeAnExistingBookEvenWhenOneUnitDoesNotFitTheFloor) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s, "4", "4.20", 100);
  ASSERT_TRUE(s.submit(f.market("ten-calls", 10), f.time).decision.ok());
  const auto room = s.snapshot()->equity - s.snapshot()->evaluation.floor;
  const auto request = f.market("hedge", 1, Side::Sell);
  const auto preview = s.preview(request, f.time);
  ASSERT_TRUE(preview.max_loss);
  EXPECT_GT(*preview.max_loss, room.prorate(1, 2));
  Quantity expected = 0;
  for (Quantity size = 1; size <= 12; ++size) {
    auto sized = request; sized.quantity = size;
    const auto candidate = s.preview(sized, f.time);
    if (candidate.decision.ok() && candidate.buying_power_after && *candidate.buying_power_after >= Money{} &&
        candidate.max_loss && *candidate.max_loss <= room.prorate(1, 2)) expected = size;
  }
  EXPECT_GT(expected, 1);
  EXPECT_EQ(preview.max_units, expected);
}
TEST(TradingPreview, SizeCanRestoreBuyingPowerWhenAPartialCloseLeavesADeficit) {
  ScriptedMarket f; auto c = config(); c.rules.buying_power = false;
  TradingSession s(c, f.time); f.seed(s, "4", "4.20", 100);
  ASSERT_TRUE(s.submit(f.market("shorts", 2, Side::Sell), f.time).decision.ok());
  const auto preview = s.preview(f.market("close", 1), f.time, 1);
  ASSERT_TRUE(preview.buying_power_after);
  EXPECT_LT(*preview.buying_power_after, Money{});
  EXPECT_EQ(preview.max_units, 2);
  const auto sized = s.preview(f.market("close", *preview.max_units), f.time, 1);
  EXPECT_GE(*sized.buying_power_after, Money{});
  EXPECT_FALSE(*sized.breaches_floor);
}
TEST(TradingPreview, ImpactBlocksPriceTheProjectionAndTheSize) {
  // An ask of 1.05 for 8 contracts; each further block of 8 costs one tick (0.05) more.
  ScriptedMarket f; auto c = config();
  c.initial_cash = m("100000"); c.rules.max_drawdown = m("6000"); c.rules.impact_ticks = 1;
  TradingSession s(c, f.time); f.seed(s, "1.00", "1.05", 8);
  // 26 contracts cost 8 x 105 + 8 x 110 + 8 x 115 + 2 x 120 + 26 x 0.65 = 2,896.90;
  // 27 would cost 3,017.55, more than half the 6,000 room.
  EXPECT_EQ(s.preview(f.market("size"), f.time).max_units, 26);
  const auto sized = s.preview(f.market("26", 26), f.time);
  EXPECT_EQ(sized.max_loss, m("2896.90"));
  EXPECT_EQ(sized.buying_power_required, m("2896.90"));
  EXPECT_EQ(sized.buying_power_after, m("100000") - m("2896.90"));
  // The fill walks the same blocks.
  ASSERT_TRUE(s.submit(f.market("26", 26), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->account.cash, m("100000") - m("2896.90"));
}
TEST(TradingPreview, ABracketEntryCountsItsExitsFeeReservation) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  auto entry = f.market("bracket");
  entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3")}, {}},
                          ExitSpec{{}, m("6")}};
  // 10,000 - 420 premium - 0.65 fee - 0.65 held for the exit pair.
  const auto preview = s.preview(entry, f.time);
  EXPECT_EQ(preview.buying_power_after, m("9578.70"));
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.available, *preview.buying_power_after);
}
TEST(TradingPreview, SellingHeldLongsIsMeasuredOnTheAccountNotAsNewShorts) {
  ScriptedMarket f; f.contract = *md::parse_osi("SPXW261022P05000000");
  TradingSession s(config(), f.time);
  s.define(f.contract, f.time);
  s.on_quotes({f.quote()}, {f.valuation(-0.5)}, f.time);
  ASSERT_TRUE(s.submit(f.market("longs", 2), f.time).decision.ok());
  // Selling both puts at 4.00 against a 4.10 mark costs 20.00 and 1.30 of fees; the account is then flat.
  const auto close = s.preview(f.limit("close", 2, "4.00", Side::Sell), f.time);
  EXPECT_EQ(close.max_loss_basis, "scenario_grid");
  EXPECT_EQ(close.max_loss, m("21.30"));
  EXPECT_FALSE(*close.breaches_floor);
  // Selling more than is held still writes shorts, and the grid shows their risk.
  const auto flip = s.preview(f.limit("flip", 3, "4.00", Side::Sell), f.time);
  EXPECT_EQ(flip.max_loss_basis, "scenario_grid");
  EXPECT_GT(*flip.max_loss, *close.max_loss);
}
TEST(TradingBuyingPower, AMarketOrderOnAOneSidedBookReservesTheSideItWouldTake) {
  ScriptedMarket f; auto c = config(); c.initial_cash = m("100000");
  TradingSession s(c, f.time); f.seed(s);
  auto armed = f.market("armed");
  armed.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrAbove, m("50")};
  ASSERT_TRUE(s.submit(armed, f.time).decision.ok());
  // An armed buy stop holds its level, the least it pays once the ask reaches it.
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("5000.65"));
  // Nobody bids now, and the ask is 0.10: it still holds its level.
  f.next();
  s.on_quotes({{f.symbol(), f.observation, f.time, std::nullopt, m("0.10"), 0, 27}}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::Armed);
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("5000.65"));
}
TEST(TradingBreach, SolvesUpAndDownWithReflectionAndKeepsMissingInputsAbsent) {
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  for (const Quantity shares : {100, -100}) {
    Ledger ledger(m("10000")); ledger.trade_stock("SPY", shares, m("100"), {});
    const auto risk = breach_risk(ledger, {}, m("10000"), m("9000"), m("9500"), time,
        md::kNanosPerMinute, {{"SPY", 100}}, {{"SPY", 0.0004}});
    EXPECT_EQ(risk.room, m("1000")); EXPECT_EQ(risk.soft_room, m("500"));
    ASSERT_EQ(risk.underlyings.size(), 1U);
    const auto& item = risk.underlyings.front();
    const auto level = shares > 0 ? item.down : item.up;
    ASSERT_TRUE(level);
    EXPECT_NEAR(level->points, shares > 0 ? -10 : 10, 1e-7);
    EXPECT_NEAR(level->percent, shares > 0 ? -10 : 10, 1e-7);
    ASSERT_TRUE(level->touch_probability);
    EXPECT_GE(*level->touch_probability, 0);
    EXPECT_LE(*level->touch_probability, 1);
    EXPECT_FALSE(shares > 0 ? item.up : item.down);
    const auto missing = breach_risk(ledger, {}, m("10000"), m("9000"), {}, time, md::kNanosPerMinute);
    EXPECT_FALSE(missing.complete);
    EXPECT_FALSE(missing.underlyings.front().up);
  }
}
TEST(TradingBreach, WithoutAPlanFloorLevelsReachTheSoftFloor) {
  ScriptedMarket f; auto c = config(); c.rules.max_drawdown = {};
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  // Held underlyings are listed without a floor, with nothing to reach.
  auto risk = s.breach({{"SPX", 0.0004}});
  EXPECT_FALSE(risk.room); EXPECT_FALSE(risk.soft_room);
  ASSERT_EQ(risk.underlyings.size(), 1U);
  EXPECT_TRUE(risk.underlyings[0].complete);
  EXPECT_EQ(risk.underlyings[0].spot, 5000);
  EXPECT_FALSE(risk.underlyings[0].down); EXPECT_FALSE(risk.underlyings[0].up);
  // A soft floor 278.70 below equity: the long calls reach it on a fall.
  Guardrails g; g.soft_floor = m("9700"); s.set_guardrails(g, f.time);
  risk = s.breach({{"SPX", 0.0004}});
  EXPECT_FALSE(risk.room);
  EXPECT_EQ(risk.soft_room, m("278.70"));
  ASSERT_EQ(risk.underlyings.size(), 1U);
  ASSERT_TRUE(risk.underlyings[0].down);
  EXPECT_LT(risk.underlyings[0].down->percent, 0);
  EXPECT_FALSE(risk.underlyings[0].up);
}
TEST(TradingBreach, StaleMarksNeverProduceAccountBreachLevelsFromFreshGreeks) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  s.submit(f.market("long"), f.time);
  f.time += 61 * md::kNanosPerSecond;
  s.on_quotes({}, {f.valuation()}, f.time);
  const auto risk = s.breach({{"SPX", 0.2}});
  EXPECT_FALSE(risk.complete);
  EXPECT_FALSE(risk.room);
  ASSERT_EQ(risk.underlyings.size(), 1U);
  EXPECT_FALSE(risk.underlyings[0].complete);
  EXPECT_FALSE(risk.underlyings[0].down);
  EXPECT_FALSE(risk.underlyings[0].up);
}
}  // namespace
}  // namespace openport::trading

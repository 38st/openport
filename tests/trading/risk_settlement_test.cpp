#include <cmath>
#include <limits>
#include <gtest/gtest.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view text) { return Money::parse(text); }
TEST(TradingRisk, OrderValidationAndExplicitNumericChecks) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  EXPECT_EQ(s.submit(f.market("unknown"), f.time).decision.code, Reason::UNKNOWN_CONTRACT);
  f.seed(s);
  auto request = f.market("day"); request.tif = TimeInForce::Day;
  EXPECT_EQ(s.submit(request, f.time).decision.code, Reason::INVALID_ORDER);
  request = f.limit("tick", 1, "4.21");
  EXPECT_EQ(s.submit(request, f.time).decision.code, Reason::INVALID_TICK);
  EXPECT_EQ(s.submit(f.market("zero", 0), f.time).decision.code, Reason::INVALID_ORDER);
  const auto max = s.submit(f.market("max", 101), f.time).decision;
  EXPECT_EQ(max.code, Reason::MAX_ORDER_CONTRACTS);
  EXPECT_EQ(max.actual, 101); EXPECT_EQ(max.limit, 100);
  const auto band = s.submit(f.limit("band", 1, "5.00"), f.time).decision;
  EXPECT_EQ(band.code, Reason::PRICE_BAND);
  EXPECT_DOUBLE_EQ(*band.actual, 0.9);
  EXPECT_DOUBLE_EQ(*band.limit, 0.82);
  EXPECT_TRUE(s.submit(f.limit("okay", 1, "4.10"), f.time).decision.ok());
  EXPECT_EQ(s.submit(f.limit("okay", 1, "4.10"), f.time).decision.code, Reason::DUPLICATE_CLIENT_ID);
  auto limits = Limits{};
  limits.price_band_relative = 0;
  limits.price_band_absolute = m("0.05");
  s.set_limits(limits, f.time);
  EXPECT_EQ(s.submit(f.market("wide"), f.time).decision.code, Reason::PRICE_BAND);
}
TEST(TradingRisk, FreshQuotesAndValuationsAreRequired) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  s.define(f.contract, f.time);
  EXPECT_EQ(s.submit(f.market("noquote"), f.time).decision.code, Reason::INVALID_QUOTE);
  s.on_quotes({f.quote()}, {}, f.time);
  EXPECT_EQ(s.submit(f.market("noval"), f.time).decision.code, Reason::MISSING_VALUATION);
  s.on_quotes({}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.submit(f.market("position"), f.time).decision.ok());
  f.time += 61 * md::kNanosPerSecond;
  EXPECT_EQ(s.submit(f.market("stale"), f.time).decision.code, Reason::STALE_QUOTE);
  EXPECT_FALSE(s.snapshot()->valuation_complete);
  EXPECT_FALSE(s.snapshot()->risk.complete);
  EXPECT_EQ(s.snapshot()->positions[0].mark, m("4.10"));
  ++f.observation;
  s.on_quotes({f.quote()}, {}, f.time);
  EXPECT_EQ(s.submit(f.market("staleval"), f.time).decision.code, Reason::MISSING_VALUATION);
}
TEST(TradingRisk, PendingOrdersReserveWorstSubsetWithoutNettingOppositeSides) {
  ScriptedMarket f;
  SessionConfig c;
  c.limits.per_underlying.dollar_delta = 600'000;
  c.limits.aggregate.dollar_delta = 600'000;
  TradingSession s(c, f.time);
  f.seed(s, "4", "4.40");
  EXPECT_TRUE(s.submit(f.limit("buy", 2, "4.10"), f.time).decision.ok());
  EXPECT_TRUE(s.submit(f.limit("sell", 2, "4.30", Side::Sell), f.time).decision.ok());
  auto risk = s.snapshot()->risk;
  EXPECT_DOUBLE_EQ(risk.aggregate.reachable.delta_low, -500'000);
  EXPECT_DOUBLE_EQ(risk.aggregate.reachable.delta_high, 500'000);
  const auto reject = s.submit(f.limit("buy2", 1, "4.10"), f.time).decision;
  EXPECT_EQ(reject.code, Reason::DELTA_LIMIT);
  EXPECT_EQ(reject.scope, "SPX");
  EXPECT_EQ(reject.actual, 750'000);
  EXPECT_EQ(reject.limit, 600'000);
  EXPECT_NEAR(s.snapshot()->risk.aggregate.delta_utilisation, 5.0 / 6, 1e-12);
}
TEST(TradingRisk, AggregateAndUnderlyingVegaLimitsHaveIndependentChecks) {
  ScriptedMarket a;
  ScriptedMarket b;
  b.contract = *md::parse_osi("XSP261022C00500000");
  SessionConfig config;
  config.limits.aggregate.vega = 300;
  config.limits.per_underlying.vega = 250;
  TradingSession s(config, a.time);
  a.seed(s); b.seed(s);
  EXPECT_TRUE(s.submit(a.limit("spx", 1, "4.10"), a.time).decision.ok());
  auto d = s.submit(b.limit("xsp", 1, "4.10"), a.time).decision;
  EXPECT_EQ(d.code, Reason::VEGA_LIMIT);
  EXPECT_EQ(d.scope, "aggregate"); EXPECT_EQ(d.actual, 400); EXPECT_EQ(d.limit, 300);
  config.limits.underlying_overrides["XSP"] = {1e6, 150};
  s.set_limits(config.limits, a.time);
  d = s.submit(b.limit("xsp2", 1, "4.10"), a.time).decision;
  EXPECT_EQ(d.code, Reason::VEGA_LIMIT); EXPECT_EQ(d.scope, "XSP");
  config.limits.aggregate.dollar_delta = 300'000;
  config.limits.aggregate.vega = 10000;
  config.limits.underlying_overrides.clear();
  s.set_limits(config.limits, a.time);
  d = s.submit(b.limit("xsp3", 1, "4.10"), a.time).decision;
  EXPECT_EQ(d.code, Reason::DELTA_LIMIT); EXPECT_EQ(d.scope, "aggregate");
}
TEST(TradingRisk, FillRecheckCancelsRemainderWhenGreeksOrValuationsChange) {
  for (const bool missing : {false, true}) {
    ScriptedMarket f;
    SessionConfig c;
    c.limits.aggregate.dollar_delta = 300'000;
    TradingSession s(c, f.time);
    f.seed(s);
    s.submit(f.limit("rest", 1, "4.10"), f.time);
    f.next();
    auto valuation = f.valuation(1);
    if (missing) valuation.delta = std::numeric_limits<double>::quiet_NaN();
    s.on_quotes({f.quote("4", "4.10")}, {valuation}, f.time);
    EXPECT_TRUE(s.snapshot()->recent_fills.empty());
    EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::RISK_CHANGED);
    EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::Cancelled);
  }
}
TEST(TradingRisk, LimitTighteningRechecksAndInvalidLimitsLeaveStateUntouched) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  f.seed(s);
  s.submit(f.limit("rest", 2, "4.10"), f.time);
  auto limits = Limits{};
  limits.max_order_contracts = 1;
  s.set_limits(limits, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::RISK_CHANGED);
  EXPECT_EQ(s.snapshot()->risk.limits_revision, 2);
  const auto before = s.snapshot_json();
  limits.aggregate.vega = -1;
  EXPECT_THROW(s.set_limits(limits, f.time), TradingError);
  EXPECT_EQ(s.snapshot_json(), before);
}
TEST(TradingRisk, ManualKillLatchAndExplicitReset) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  f.seed(s);
  s.submit(f.limit("rest", 1, "4.10"), f.time);
  s.trip_kill("operator pause", f.time);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::KILL_SWITCH);
  EXPECT_EQ(s.submit(f.market("blocked"), f.time).decision.code, Reason::KILL_SWITCH);
  EXPECT_THROW(s.reset_kill("  ", f.time), TradingError);
  s.reset_kill("reviewed", f.time);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
  EXPECT_TRUE(s.submit(f.market("allowed"), f.time).decision.ok());
}
TEST(TradingRisk, DailyLossCancelsBeforeMatchingAndResetImmediatelyRetrips) {
  ScriptedMarket f;
  SessionConfig c;
  c.limits.max_daily_loss = m("100");
  TradingSession s(c, f.time);
  f.seed(s);
  s.submit(f.market("position"), f.time);
  s.submit(f.limit("rest", 1, "4.10"), f.time);
  f.next();
  s.on_quotes({f.quote("1", "1.20")}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "DAILY_LOSS");
  EXPECT_EQ(s.snapshot()->risk.daily_loss, m("310.65"));
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 1);
  EXPECT_EQ(s.snapshot()->recent_orders[1].reason.code, Reason::KILL_SWITCH);
  EXPECT_EQ(s.reset_kill("reviewed", f.time).decision.code, Reason::DAILY_LOSS);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.roll_day(f.time).decision.code, Reason::INVALID_TIME);
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  ++f.observation;
  EXPECT_EQ(s.roll_day(f.time).decision.code, Reason::STALE_QUOTE);
  s.on_quotes({f.quote("1", "1.20")}, {f.valuation()}, f.time);
  s.roll_day(f.time);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->risk.daily_loss, Money{});
  s.reset_kill("new daily baseline approved", f.time);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
}
TEST(TradingRisk, ProposedFillIncludesSpreadAndFeesInDailyLossCheck) {
  ScriptedMarket f;
  SessionConfig c;
  c.limits.max_daily_loss = m("10");
  TradingSession s(c, f.time);
  f.seed(s);
  s.submit(f.market("tooexpensive"), f.time);
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::RISK_CHANGED);
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.actual, 10.65);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
}
TEST(TradingRisk, GreekUnitsAndScenarioSignsForCallsPutsLongsShorts) {
  ScriptedMarket f;
  for (const auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
    for (const Quantity direction : {Quantity{-1}, Quantity{1}}) {
      f.contract.type = type;
      Ledger ledger(m("100000"));
      ledger.fill(f.contract, direction, m("4.20"), m("0.65"));
      const auto v = f.valuation();
      const std::map<std::string, Valuation> values{{f.symbol(), v}};
      const auto risk = portfolio_risk(ledger, {}, {{f.symbol(), f.contract}}, values, {}, f.time);
      EXPECT_DOUBLE_EQ(risk.aggregate.position.dollar_delta, static_cast<double>(direction) * 250000);
      EXPECT_DOUBLE_EQ(risk.aggregate.position.dollar_gamma_1pct, static_cast<double>(direction) * 25000);
      EXPECT_DOUBLE_EQ(risk.aggregate.position.vega, static_cast<double>(direction) * 200);
      EXPECT_DOUBLE_EQ(risk.aggregate.position.theta, static_cast<double>(direction) * -10);
      ScenarioConfig config;
      config.spot_percent = {-1, 0, 1}; config.vol_points = {0, 5};
      const auto grid = scenario_grid(ledger, values, config, f.time, md::kNanosPerMinute);
      ASSERT_TRUE(grid.complete);
      EXPECT_EQ(grid.cells[2].pnl, 0);
      EXPECT_GT(grid.cells[3].pnl * static_cast<double>(direction), 0);
      const double call_sign = type == pricing::OptionType::Call ? 1 : -1;
      EXPECT_GT(grid.cells[4].pnl * static_cast<double>(direction) * call_sign, 0);
      EXPECT_LT(grid.cells[0].pnl * static_cast<double>(direction) * call_sign, 0);
      config.vol_points = {-50};
      const auto clamped = scenario_grid(ledger, values, config, f.time, md::kNanosPerMinute);
      EXPECT_TRUE(clamped.cells[0].clamped);
      EXPECT_TRUE(std::isfinite(clamped.cells[0].pnl));
      const auto missing = scenario_grid(ledger, {}, config, f.time, md::kNanosPerMinute);
      EXPECT_FALSE(missing.complete);
      config.spot_percent = {-100};
      EXPECT_THROW((void)scenario_grid(ledger, values, config, f.time, md::kNanosPerMinute), TradingError);
    }
  }
}
TEST(TradingSettlement, AmSettlementRequiresExplicitValueAndWorksWhileKilled) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPX260923C05000000");
  TradingSession s({}, f.time);
  f.seed(s);
  s.submit(f.market("position"), f.time);
  s.trip_kill("expiry review", f.time);
  f.time = f.contract.expiry_time();
  s.on_quotes({}, {}, f.time);
  ASSERT_EQ(s.snapshot()->positions.size(), 1);
  EXPECT_TRUE(s.snapshot()->positions[0].awaiting_settlement);
  EXPECT_EQ(s.snapshot()->account.cash, m("99579.35"));
  EXPECT_TRUE(s.settle(f.symbol(), m("5010"), f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->account.cash, m("100579.35"));
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
}
TEST(TradingRisk, RolloverUsesNewYorkDateAcrossDstTransitionMidnight) {
  const auto initial = *md::parse_datetime("2026-03-07T12:00:00Z", md::Zone::Utc);
  TradingSession s({}, initial);
  // 04:30 UTC on spring-forward date is still 23:30 EST the previous date.
  const auto previous_day = *md::parse_datetime("2026-03-08T04:30:00Z", md::Zone::Utc);
  EXPECT_EQ(s.roll_day(previous_day).decision.code, Reason::INVALID_TIME);
  const auto new_day = *md::parse_datetime("2026-03-08T05:00:00Z", md::Zone::Utc);
  EXPECT_TRUE(s.roll_day(new_day).decision.ok());
}
TEST(TradingSettlement, AwaitingAndItmOtmCashSettlementForBothSignsAndRights) {
  for (const auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
    for (const Side side : {Side::Buy, Side::Sell}) {
      for (const bool itm : {false, true}) {
        ScriptedMarket f;
        f.contract.type = type;
        f.contract.expiry = {2026, 9, 22};
        TradingSession s({}, f.time);
        f.seed(s);
        s.submit(f.market("position", 1, side), f.time);
        EXPECT_EQ(s.settle(f.symbol(), m("5000"), f.time).decision.code, Reason::INVALID_SETTLEMENT);
        const auto expiry = f.contract.expiry_time();
        s.on_quotes({}, {}, expiry);
        EXPECT_TRUE(s.snapshot()->positions[0].awaiting_settlement);
        EXPECT_FALSE(s.snapshot()->valuation_complete);
        EXPECT_EQ(s.snapshot()->positions[0].mark, m("4.10"));
        EXPECT_EQ(s.submit(f.market("expired"), expiry).decision.code, Reason::EXPIRED);
        const auto settle_price = !itm ? m("5000") : type == pricing::OptionType::Call ? m("5010") : m("4990");
        const auto before = s.snapshot();
        s.settle(f.symbol(), settle_price, expiry);
        const auto sign = side == Side::Buy ? 1 : -1;
        EXPECT_TRUE(s.snapshot()->positions.empty());
        EXPECT_EQ(s.snapshot()->account.cash, before->account.cash + m(itm ? "1000" : "0") * sign);
        EXPECT_EQ(s.snapshot()->account.cash, m("100000") + s.snapshot()->account.realised - s.snapshot()->account.fees);
        EXPECT_EQ(s.settle(f.symbol(), settle_price, expiry).decision.code, Reason::ALREADY_SETTLED);
      }
    }
  }
}
}  // namespace
}  // namespace openport::trading

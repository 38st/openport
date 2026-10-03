#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig config(AccountRules rules) {
  SessionConfig c;
  c.initial_cash = m("10000");
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
/// A funded account: the floor locks at the starting balance; a payout needs
/// `days` days of $50 net realised profit and takes up to half the profit.
AccountRules funded(std::string_view max_drawdown = "1000", std::int64_t days = 1) {
  AccountRules r;
  r.plan = "Funded test";
  r.phase = Phase::Funded;
  r.max_drawdown = m(max_drawdown);
  r.lock_balance = m("10000");
  r.payouts.qualifying_profit = m("50");
  r.payouts.qualifying_days = days;
  r.payouts.withdrawal_percent = 50;
  r.payouts.split_percent = 80;
  r.payouts.minimum = m("10");
  r.payouts.caps = {m("60"), m("100")};
  return r;
}
void quote(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask) {
  f.next();
  s.on_quotes({f.quote(bid, ask)}, {f.valuation()}, f.time);
}
/// Buy five at 4.20, mark up to 4.40/4.60 and sell at 4.40: $100 gross, $93.50 net.
void winning_trade(TradingSession& s, ScriptedMarket& f) {
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.submit(f.market("win-open-" + std::to_string(f.observation), 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");
  ASSERT_TRUE(s.submit(f.market("win-close-" + std::to_string(f.observation), 5, Side::Sell), f.time).decision.ok());
}
void next_day(TradingSession& s, ScriptedMarket& f, md::Date day) {
  f.time = md::new_york_to_utc(day, 10, 0);
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
}

// Exact net day profit from one round trip, including both fees.
void profit_day(TradingSession& s, ScriptedMarket& f, std::string_view profit) {
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.submit(f.market("day-open-" + std::to_string(f.observation)), f.time).decision.ok());
  const auto bid = m("4.20") + (m(profit) + s.config().fee_per_contract * 2).prorate(1, 100);
  quote(s, f, bid.str(), (bid + m("0.20")).str());
  ASSERT_TRUE(s.submit(f.market("day-close-" + std::to_string(f.observation), 1, Side::Sell), f.time).decision.ok());
}

TEST(TradingFunded, ConsistencyCountsNetDaysIncludingTodayAndRestartsAtThePayoutDay) {
  ScriptedMarket f;
  auto rules = funded("5000");
  rules.payouts.consistency_percents = {40};
  TradingSession s(config(rules), f.time);
  f.seed(s);
  profit_day(s, f, "300");
  // Qualifying days block first, even though consistency is also unmet.
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.limit, 1);
  next_day(s, f, {2026, 9, 23});
  auto q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.consistency_percent, 40);
  EXPECT_EQ(q.cycle_profit, m("300"));
  EXPECT_EQ(q.best_day, m("300"));
  EXPECT_EQ(q.best_day_date, (md::Date{2026, 9, 22}));
  EXPECT_EQ(q.consistency_needed, m("450"));
  EXPECT_EQ(q.blocked.code, Reason::PAYOUT_NOT_ELIGIBLE);
  EXPECT_EQ(q.blocked.actual, 100);
  EXPECT_EQ(q.blocked.limit, 40);
  EXPECT_NE(q.blocked.message.find("consistency"), std::string::npos);
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_NOT_ELIGIBLE);
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 24});
  profit_day(s, f, "250");  // Today's $250 counts before rollover: exactly 40%.
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, m("750"));
  EXPECT_EQ(q.consistency_needed, Money{});
  ASSERT_TRUE(q.blocked.ok());
  ASSERT_TRUE(s.request_payout(m("10"), f.time).decision.ok());
  q = payout_quote(*s.snapshot(), rules);
  // The request day belongs to the new cycle in full, as with qualifying_days.
  EXPECT_EQ(q.cycle_profit, m("250"));
  EXPECT_EQ(q.best_day, m("250"));
  EXPECT_EQ(q.best_day_date, (md::Date{2026, 9, 24}));
  next_day(s, f, {2026, 9, 25});
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 28});
  profit_day(s, f, "175");
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, m("625"));
  EXPECT_EQ(q.best_day, m("250"));
  EXPECT_EQ(q.consistency_needed, Money{});
  ASSERT_TRUE(q.blocked.ok()); // The old $300 best would still block this payout.
  ASSERT_TRUE(s.request_payout(m("10"), f.time).decision.ok());
}

TEST(TradingFunded, ConsistencyEscalatesByPayoutNumberAndRepeatsTheLastPercent) {
  ScriptedMarket f;
  auto rules = funded();
  rules.payouts.consistency_percents = {20, 25, 30};
  TradingSession s(config(rules), f.time);
  f.seed(s);
  const std::vector<md::Date> dates{{2026,9,23}, {2026,9,24}, {2026,9,25}, {2026,9,28}, {2026,9,29},
      {2026,9,30}, {2026,10,1}, {2026,10,2}, {2026,10,5}, {2026,10,6}, {2026,10,7}, {2026,10,8}, {2026,10,9}};
  std::size_t day = 0;
  for (const auto& [percent, count] : {std::pair{20, 5}, {25, 4}, {30, 4}}) {
    EXPECT_EQ(payout_quote(*s.snapshot(), rules).consistency_percent, percent);
    for (int i = 0; i < count; ++i) {
      profit_day(s, f, "60");
      next_day(s, f, dates.at(day++));
    }
    ASSERT_TRUE(s.request_payout(m("10"), f.time).decision.ok());
    EXPECT_EQ(payout_quote(*s.snapshot(), rules).cycle_profit, Money{});
    EXPECT_FALSE(payout_quote(*s.snapshot(), rules).best_day);
  }
  EXPECT_EQ(payout_quote(*s.snapshot(), rules).consistency_percent, 30);
}

TEST(TradingFunded, ConsistencyIncludesLossesAndRoundsAdditionalProfitUpToCents) {
  ScriptedMarket f;
  auto rules = funded();
  rules.payouts.consistency_percents = {30};
  auto c = config(rules);
  c.fee_per_contract = m("0.65005");
  TradingSession s(c, f.time);
  f.seed(s);
  profit_day(s, f, "100.0001");
  next_day(s, f, {2026, 9, 23});
  profit_day(s, f, "-0.0001");
  auto q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, m("100"));
  EXPECT_EQ(q.consistency_needed, m("233.34"));
  profit_day(s, f, "-100");
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, Money{});
  EXPECT_EQ(q.consistency_needed, m("333.34"));
  EXPECT_FALSE(q.blocked.actual);
  profit_day(s, f, "-50");
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, m("-50"));
  EXPECT_EQ(q.consistency_needed, m("383.34"));
  EXPECT_EQ(q.blocked.code, Reason::PAYOUT_NOT_ELIGIBLE);
  EXPECT_FALSE(q.blocked.actual);
}

TEST(TradingFunded, ConsistencyDoesNotRoundAwayAMicroDollarBreach) {
  ScriptedMarket f;
  auto rules = funded();
  rules.payouts.consistency_percents = {50};
  auto c = config(rules);
  c.fee_per_contract = m("0.65005");
  TradingSession s(c, f.time);
  f.seed(s);
  profit_day(s, f, "100.0001");
  next_day(s, f, {2026, 9, 23});
  profit_day(s, f, "99.9999");
  auto q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, m("200"));
  EXPECT_EQ(q.consistency_needed, m("0.01"));
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_NOT_ELIGIBLE);
  profit_day(s, f, "0.0002");
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, m("200.0002"));
  EXPECT_EQ(q.consistency_needed, Money{});
  EXPECT_TRUE(q.blocked.ok());
}

TEST(TradingFunded, ConsistencyValidatesEveryPercent) {
  for (const auto percent : {0, -1, 101}) {
    auto rules = funded();
    rules.payouts.consistency_percents = {40, percent};
    try { validate_rules(rules); FAIL(); }
    catch (const TradingError& e) { EXPECT_EQ(e.code(), Reason::INVALID_RULES); }
  }
  auto rules = funded();
  rules.payouts.consistency_percents = {1, 100};
  EXPECT_NO_THROW(validate_rules(rules));
  rules.payouts.consistency_percents.resize(65, 40);
  EXPECT_THROW(validate_rules(rules), TradingError);
}

TEST(TradingFunded, BufferCapsTheFirstTwoPayoutsThenLifts) {
  ScriptedMarket f;
  auto rules = funded("100");
  rules.payouts.withdrawal_percent = 100;
  rules.payouts.caps.clear();
  rules.payouts.buffer = m("250");
  rules.payouts.buffer_payouts = 2;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 23});
  auto q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.buffer_balance, m("10250"));
  EXPECT_EQ(q.maximum, Money{});
  EXPECT_EQ(q.trader_share, Money{});
  EXPECT_EQ(q.blocked.message, "Equity must stay above the payout buffer of $10250.00");
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_NOT_ELIGIBLE);
  profit_day(s, f, "100");
  next_day(s, f, {2026, 9, 24});
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.withdrawable, m("300"));
  EXPECT_EQ(q.maximum, m("50"));
  EXPECT_EQ(q.trader_share, m("40"));
  EXPECT_EQ(s.request_payout(m("50.01"), f.time).decision.code, Reason::INVALID_PAYOUT);
  ASSERT_TRUE(s.request_payout(m("50"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->equity, m("10250")); // Equality is allowed by the buffer cap.
  EXPECT_EQ(payout_quote(*s.snapshot(), rules).buffer_balance, m("10250"));
  profit_day(s, f, "100");
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(payout_quote(*s.snapshot(), rules).maximum, m("100"));
  ASSERT_TRUE(s.request_payout(m("100"), f.time).decision.ok());
  EXPECT_FALSE(payout_quote(*s.snapshot(), rules).buffer_balance);
  profit_day(s, f, "100");
  next_day(s, f, {2026, 9, 28});
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.maximum, m("349.99")); // The locked floor still applies.
  ASSERT_TRUE(s.request_payout(q.maximum, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->equity, m("10000.01"));
}

TEST(TradingFunded, BufferUsesTheAttemptsStartingBalanceAndWholeCents) {
  ScriptedMarket f;
  auto rules = funded();
  rules.max_drawdown = Money{};
  rules.lock_balance = Money{};
  rules.payouts.withdrawal_percent = 100;
  rules.payouts.caps.clear();
  rules.payouts.buffer = m("20.000001");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.reset_account(m("20000"), rules, "different start", f.time).decision.ok());
  profit_day(s, f, "100");
  next_day(s, f, {2026, 9, 23});
  auto q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.buffer_balance, m("20020.000001"));
  EXPECT_EQ(q.maximum, m("79.99"));
  ASSERT_TRUE(s.request_payout(q.maximum, f.time).decision.ok());
  EXPECT_EQ(payout_quote(*s.snapshot(), rules).buffer_balance, m("20020.000001"));
  profit_day(s, f, "100");
  next_day(s, f, {2026, 9, 24});
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.maximum, m("100"));
  ASSERT_TRUE(s.request_payout(q.maximum, f.time).decision.ok());
  EXPECT_TRUE(payout_quote(*s.snapshot(), rules).buffer_balance); // Zero payouts means every cycle.
}

TEST(TradingFunded, BufferValidatesAmountAndPayoutCount) {
  for (const auto count : {-1, 101}) {
    auto rules = funded();
    rules.payouts.buffer_payouts = count;
    try { validate_rules(rules); FAIL(); }
    catch (const TradingError& e) { EXPECT_EQ(e.code(), Reason::INVALID_RULES); }
  }
  auto rules = funded();
  rules.payouts.buffer = m("-0.000001");
  EXPECT_THROW(validate_rules(rules), TradingError);
  rules.payouts.buffer = Money{};
  rules.payouts.buffer_payouts = 100;
  EXPECT_NO_THROW(validate_rules(rules));
  ScriptedMarket f;
  TradingSession s(config(rules), f.time);
  EXPECT_FALSE(payout_quote(*s.snapshot(), rules).buffer_balance); // Zero buffer disables even with a count.
}

TEST(TradingFunded, FloorLocksAtTheLockBalanceAndStopsTrailing) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_drawdown = m("100");
  rules.lock_balance = m("10000");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("9900"));
  EXPECT_FALSE(s.snapshot()->evaluation.floor_locked);
  quote(s, f, "4.40", "4.60");  // equity 10146.75: peak - 100 passes the lock balance
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("10000"));
  EXPECT_TRUE(s.snapshot()->evaluation.floor_locked);
  quote(s, f, "4.60", "4.80");  // a new high no longer moves the floor
  EXPECT_EQ(s.snapshot()->evaluation.peak, m("10246.75"));
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("10000"));
  quote(s, f, "4.20", "4.40");  // 10046.75 would breach a trailing floor of 10146.75
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  quote(s, f, "4.00", "4.20");
  const auto e = s.snapshot()->evaluation;
  EXPECT_EQ(e.status, EvaluationStatus::Failed);
  EXPECT_NE(e.decision.find("drawdown floor $10000.00"), std::string::npos) << e.decision;
}

TEST(TradingFunded, QualifyingDaysCountNetRealisedProfitOnFundedAccountsOnly) {
  ScriptedMarket f;
  TradingSession s(config(funded("1000", 8)), f.time);
  f.seed(s);
  winning_trade(s, f);
  next_day(s, f, {2026, 9, 23});
  auto e = s.snapshot()->evaluation;
  ASSERT_EQ(e.days.size(), 1);
  EXPECT_EQ(e.days[0].realised, m("93.50"));
  EXPECT_TRUE(e.days[0].qualifying);
  EXPECT_EQ(e.qualifying_days, 1);
  // A losing day and a small winning day do not qualify.
  ASSERT_TRUE(s.submit(f.market("lose-open", 5), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("lose-close", 5, Side::Sell), f.time).decision.ok());
  next_day(s, f, {2026, 9, 24});
  ASSERT_TRUE(s.submit(f.market("small-open", 1), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");
  ASSERT_TRUE(s.submit(f.market("small-close", 1, Side::Sell), f.time).decision.ok());
  next_day(s, f, {2026, 9, 25});
  e = s.snapshot()->evaluation;
  ASSERT_EQ(e.days.size(), 3);
  EXPECT_EQ(e.days[1].realised, m("-106.50"));
  EXPECT_FALSE(e.days[1].qualifying);
  EXPECT_EQ(e.days[2].realised, m("18.70"));
  EXPECT_FALSE(e.days[2].qualifying);
  EXPECT_EQ(e.qualifying_days, 1);

  // The same winning day on an evaluation account records its P&L but never qualifies.
  ScriptedMarket g;
  auto evaluation = funded();
  evaluation.phase = Phase::Evaluation;
  TradingSession t(config(evaluation), g.time);
  g.seed(t);
  winning_trade(t, g);
  next_day(t, g, {2026, 9, 23});
  EXPECT_EQ(t.snapshot()->evaluation.days.at(0).realised, m("93.50"));
  EXPECT_FALSE(t.snapshot()->evaluation.days.at(0).qualifying);
  EXPECT_EQ(t.snapshot()->evaluation.qualifying_days, 0);
}

TEST(TradingFunded, PayoutWithdrawsWithinTheRulesAndStartsANewCycle) {
  ScriptedMarket f;
  TradingSession s(config(funded()), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");
  auto result = s.request_payout(m("10"), f.time);
  EXPECT_EQ(result.decision.code, Reason::PAYOUT_NOT_ELIGIBLE);
  EXPECT_NE(result.decision.message.find("Close every position"), std::string::npos);
  ASSERT_TRUE(s.submit(f.market("close", 5, Side::Sell), f.time).decision.ok());
  result = s.request_payout(m("10"), f.time);
  EXPECT_EQ(result.decision.code, Reason::PAYOUT_NOT_ELIGIBLE);
  EXPECT_EQ(result.decision.limit, 1);
  next_day(s, f, {2026, 9, 23});

  auto quote_now = payout_quote(*s.snapshot(), s.config().rules);
  EXPECT_TRUE(quote_now.blocked.ok()) << quote_now.blocked.message;
  EXPECT_EQ(quote_now.number, 1);
  EXPECT_EQ(quote_now.profit, m("93.50"));
  EXPECT_EQ(quote_now.withdrawable, m("46.75"));
  EXPECT_EQ(quote_now.cap, m("60"));
  EXPECT_EQ(quote_now.maximum, m("46.75"));
  EXPECT_EQ(quote_now.trader_share, m("37.40"));
  EXPECT_EQ(s.request_payout(m("5"), f.time).decision.code, Reason::INVALID_PAYOUT);
  result = s.request_payout(m("46.76"), f.time);
  EXPECT_EQ(result.decision.code, Reason::INVALID_PAYOUT);
  EXPECT_EQ(result.decision.limit, 46.75);

  const auto before = s.snapshot();
  EXPECT_EQ(before->evaluation.peak, m("10146.75"));
  ASSERT_TRUE(s.request_payout(m("46.75"), f.time).decision.ok());
  auto snap = s.snapshot();
  EXPECT_EQ(snap->account.cash, m("10046.75"));
  EXPECT_EQ(snap->account.realised, before->account.realised);
  // Not a loss: the day's baseline and the unlocked trailing peak move with it.
  EXPECT_EQ(snap->start_of_day_equity, m("10046.75"));
  EXPECT_EQ(snap->risk.daily_loss, Money{});
  EXPECT_EQ(snap->evaluation.peak, m("10100"));
  EXPECT_EQ(snap->evaluation.floor, m("9100"));
  ASSERT_EQ(snap->evaluation.payouts.size(), 1);
  const auto payout = snap->evaluation.payouts[0];
  EXPECT_EQ(payout.number, 1);
  EXPECT_EQ(payout.time, f.time);
  EXPECT_EQ(payout.day, (md::Date{2026, 9, 23}));
  EXPECT_EQ(payout.amount, m("46.75"));
  EXPECT_EQ(payout.trader_share, m("37.40"));
  EXPECT_EQ(payout.balance, m("10093.50"));
  EXPECT_EQ(snap->evaluation.qualifying_days, 0);
  EXPECT_EQ(snap->evaluation.cycle_started, f.time);
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_NOT_ELIGIBLE);

  // Each day counts once, toward the cycle in progress when it closes: a win
  // after the request on the payout day starts the next cycle.
  winning_trade(s, f);
  next_day(s, f, {2026, 9, 24});
  EXPECT_TRUE(s.snapshot()->evaluation.days.back().qualifying);
  EXPECT_EQ(s.snapshot()->evaluation.days.back().day, (md::Date{2026, 9, 23}));
  EXPECT_EQ(s.snapshot()->evaluation.qualifying_days, 1);
  winning_trade(s, f);
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.qualifying_days, 2);

  quote_now = payout_quote(*s.snapshot(), s.config().rules);
  EXPECT_EQ(quote_now.profit, m("233.75"));
  EXPECT_EQ(quote_now.withdrawable, m("116.87"));
  EXPECT_EQ(quote_now.cap, m("100"));
  EXPECT_EQ(quote_now.maximum, m("100"));
  ASSERT_TRUE(s.request_payout(m("100"), f.time).decision.ok());
  snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.payouts.at(1).number, 2);
  EXPECT_EQ(snap->evaluation.payouts.at(1).trader_share, m("80"));
  EXPECT_EQ(snap->account.cash, m("10133.75"));
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Active);
}

TEST(TradingFunded, APayoutBeforeRolloverBelongsToTheUnrolledDay) {
  ScriptedMarket f;
  auto rules = funded();
  rules.drawdown_mode = DrawdownMode::EndOfDay;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  winning_trade(s, f);
  next_day(s, f, {2026, 9, 23});
  EXPECT_EQ(s.snapshot()->evaluation.peak, m("10093.50"));
  winning_trade(s, f);  // closes at 10187.00
  // After midnight, before the next rollover: the request joins Sep 23.
  f.time = md::new_york_to_utc({2026, 9, 24}, 0, 30);
  ASSERT_TRUE(s.request_payout(m("60"), f.time).decision.ok());
  auto e = s.snapshot()->evaluation;
  EXPECT_EQ(e.payouts.at(0).day, (md::Date{2026, 9, 23}));
  EXPECT_EQ(e.day_close_equity, m("10127"));
  EXPECT_EQ(e.peak, m("10033.50"));
  next_day(s, f, {2026, 9, 24});
  e = s.snapshot()->evaluation;
  // The close ratchets net of the withdrawal: $1,000 of room remains, as without it.
  EXPECT_EQ(e.peak, m("10127"));
  EXPECT_EQ(e.floor, m("9127"));
  EXPECT_EQ(s.snapshot()->equity - e.floor, m("1000"));
  const auto& day = e.days.back();
  EXPECT_EQ(day.day, (md::Date{2026, 9, 23}));
  EXPECT_EQ(day.open_equity, m("10033.50"));
  EXPECT_EQ(day.close_equity, m("10127"));
  EXPECT_EQ(day.realised, m("93.50"));
  EXPECT_TRUE(day.qualifying);
  EXPECT_EQ(e.qualifying_days, 1);  // Sep 23 closed after the request, so it starts the next cycle
}

TEST(TradingFunded, PayoutsLeaveEquityAboveALockedFloor) {
  ScriptedMarket f;
  auto rules = funded("100");
  rules.payouts.withdrawal_percent = 100;
  rules.payouts.caps.clear();
  TradingSession s(config(rules), f.time);
  f.seed(s);
  winning_trade(s, f);
  ASSERT_TRUE(s.snapshot()->evaluation.floor_locked);
  next_day(s, f, {2026, 9, 23});
  const auto q = payout_quote(*s.snapshot(), s.config().rules);
  EXPECT_EQ(q.withdrawable, m("93.50"));
  EXPECT_FALSE(q.cap);
  EXPECT_EQ(q.maximum, m("93.49"));
  EXPECT_EQ(s.request_payout(m("93.50"), f.time).decision.code, Reason::INVALID_PAYOUT);
  ASSERT_TRUE(s.request_payout(m("93.49"), f.time).decision.ok());
  const auto e = s.snapshot()->evaluation;
  EXPECT_EQ(e.status, EvaluationStatus::Active);
  EXPECT_EQ(e.floor, m("10000"));
  EXPECT_EQ(e.peak, m("10146.75"));
  EXPECT_EQ(s.snapshot()->equity, m("10000.01"));
}

TEST(TradingFunded, PayoutsNeedAFundedAccountAndWholeCents) {
  ScriptedMarket f;
  TradingSession s(config({}), f.time);
  f.seed(s);
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_UNAVAILABLE);
  EXPECT_FALSE(payout_quote(*s.snapshot(), s.config().rules).funded);
  for (const auto* amount : {"0", "-1", "10.001"}) {
    try {
      (void)s.request_payout(m(amount), f.time);
      ADD_FAILURE() << "expected INVALID_PAYOUT for " << amount;
    } catch (const TradingError& error) { EXPECT_EQ(error.code(), Reason::INVALID_PAYOUT); }
  }
  auto invalid = funded();
  invalid.payouts.split_percent = 101;
  EXPECT_THROW(validate_rules(invalid), TradingError);
  invalid = funded();
  invalid.payouts.caps = {m("0")};
  EXPECT_THROW(validate_rules(invalid), TradingError);
  invalid = funded();
  invalid.profit_target = m("100");  // a funded account never passes
  EXPECT_THROW(validate_rules(invalid), TradingError);
  invalid = funded();
  invalid.payouts.qualifying_days = 0;
  EXPECT_THROW(validate_rules(invalid), TradingError);
}

class CapturingJournal final : public Journal {
 public:
  struct Entry { Timestamp time; std::string type; std::string payload; };
  std::vector<Entry> entries;
  void append(Timestamp time, std::string_view type, std::string_view payload) override {
    entries.push_back({time, std::string(type), std::string(payload)});
  }
  std::uint64_t sequence() const override { return entries.size(); }
  std::string head() const override { return std::string(64, '0'); }
};
/// Rewrite a transaction as the schema 2 record written before funded accounts
/// and multi-leg orders.
std::string earlier_schema_two(std::string_view payload) {
  auto j = nlohmann::json::parse(payload);
  for (auto* e : {&j["state"]["evaluation"], &j["snapshot"]["evaluation"]}) {
    for (const auto* key : {"floor_locked", "day_open_realised", "qualifying_days", "cycle_started", "payouts"}) e->erase(key);
    for (auto& day : (*e)["days"]) { day.erase("realised"); day.erase("qualifying"); }
  }
  for (const auto* key : {"phase", "lock_balance", "payouts"}) j["state"]["config"]["rules"].erase(key);
  for (auto& order : j["state"]["orders"]) order["request"].erase("legs");
  for (const auto* list : {"recent_orders", "open_orders"})
    for (auto& order : j["snapshot"][list]) order["request"].erase("legs");
  return j.dump();
}

TEST(TradingFunded, EarlierSchemaTwoJournalsRecoverAsEvaluations) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-funded-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto current = (directory / "current.jsonl").string();
  const auto path = (directory / "earlier.jsonl").string();
  ScriptedMarket f;
  auto capture = std::make_shared<CapturingJournal>();
  AccountRules rules;
  rules.plan = "Earlier";
  rules.max_drawdown = m("1000");
  {
    TradingSession s(config(rules), f.time, FileJournal::create(current));
    f.seed(s);
    winning_trade(s, f);
    next_day(s, f, {2026, 9, 23});
  }
  // Every state whole, as schema 2 wrote it, to rewrite as the earlier schema 2.
  TradingSession::expand(FileJournal::read(current), *capture);
  {
    auto file = FileJournal::create(path);
    for (const auto& entry : capture->entries) file->append(entry.time, entry.type, earlier_schema_two(entry.payload));
  }
  auto s = TradingSession::recover(FileJournal::read(path), FileJournal::resume(path));
  EXPECT_EQ(s.config().rules.phase, Phase::Evaluation);
  EXPECT_FALSE(s.snapshot()->recent_orders.empty());
  EXPECT_TRUE(s.snapshot()->recent_orders.front().request.legs.empty());
  EXPECT_EQ(s.config().rules.lock_balance, Money{});
  auto e = s.snapshot()->evaluation;
  EXPECT_FALSE(e.floor_locked);
  EXPECT_TRUE(e.payouts.empty());
  EXPECT_EQ(e.days.at(0).realised, Money{});
  EXPECT_FALSE(e.days.at(0).qualifying);
  EXPECT_EQ(e.cycle_started, 0);
  EXPECT_FALSE(payout_quote(*s.snapshot(), s.config().rules).funded);
  // The first new transaction starts the payout cycle with the attempt.
  quote(s, f, "4.00", "4.20");
  e = s.snapshot()->evaluation;
  EXPECT_EQ(e.cycle_started, e.started);
  EXPECT_GT(e.started, 0);
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_UNAVAILABLE);
  std::filesystem::remove_all(directory);
}

TEST(TradingFunded, JournalRecoveryRestoresPayoutsAndQualifyingDays) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-funded-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "funded.jsonl").string();
  ScriptedMarket f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(config(funded("100")), f.time, journal);
    f.seed(s);
    winning_trade(s, f);
    next_day(s, f, {2026, 9, 23});
    ASSERT_TRUE(s.request_payout(m("40"), f.time).decision.ok());
    winning_trade(s, f);
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto s = TradingSession::recover(FileJournal::read(path, head), FileJournal::resume(path));
  EXPECT_EQ(s.snapshot_json(), expected);
  const auto e = s.snapshot()->evaluation;
  EXPECT_TRUE(e.floor_locked);
  ASSERT_EQ(e.payouts.size(), 1);
  EXPECT_EQ(e.payouts[0].amount, m("40"));
  EXPECT_EQ(e.days.at(0).realised, m("93.50"));
  EXPECT_TRUE(e.days.at(0).qualifying);
  EXPECT_EQ(s.config().rules.payouts.caps.size(), 2);
  EXPECT_EQ(s.config().rules.phase, Phase::Funded);
  const auto capture = std::make_shared<CapturingJournal>();
  TradingSession::expand(FileJournal::read(path), *capture);
  for (const auto& entry : capture->entries) {
    const auto recorded = nlohmann::json::parse(entry.payload)["state"]["config"]["rules"]["payouts"];
    EXPECT_EQ(recorded.size(), 6); // The original funded journal fields and bytes.
    EXPECT_FALSE(recorded.contains("consistency_percents"));
    EXPECT_FALSE(recorded.contains("buffer"));
    EXPECT_FALSE(recorded.contains("buffer_payouts"));
  }
  EXPECT_TRUE(s.config().rules.payouts.consistency_percents.empty());
  std::filesystem::remove_all(directory);
}

TEST(TradingFunded, ConsistencyJournalRecoversRulesAndTheCycle) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-consistency-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "funded.jsonl").string();
  auto rules = funded();
  rules.payouts.consistency_percents = {50, 60};
  rules.payouts.buffer = m("50");
  rules.payouts.buffer_payouts = 2;
  ScriptedMarket f;
  std::string expected;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(path));
    f.seed(s);
    winning_trade(s, f);
    next_day(s, f, {2026, 9, 23});
    winning_trade(s, f);
    next_day(s, f, {2026, 9, 24});
    ASSERT_TRUE(s.request_payout(m("40"), f.time).decision.ok());
    winning_trade(s, f);
    next_day(s, f, {2026, 9, 25});
    expected = s.snapshot_json();
  }
  auto s = TradingSession::recover(FileJournal::read(path), FileJournal::resume(path));
  EXPECT_EQ(s.snapshot_json(), expected);
  EXPECT_EQ(s.config().rules, rules);
  const auto q = payout_quote(*s.snapshot(), s.config().rules);
  EXPECT_EQ(q.consistency_percent, 60);
  EXPECT_EQ(q.buffer_balance, m("10050"));
  EXPECT_EQ(q.cycle_profit, m("93.50"));
  EXPECT_EQ(q.best_day_date, (md::Date{2026, 9, 24}));
  EXPECT_EQ(q.consistency_needed, m("62.34"));
  winning_trade(s, f);
  EXPECT_TRUE(payout_quote(*s.snapshot(), rules).blocked.ok());
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace openport::trading

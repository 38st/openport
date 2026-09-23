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
/// Rewrite a transaction as the schema 2 record written before funded accounts.
std::string before_payouts(std::string_view payload) {
  auto j = nlohmann::json::parse(payload);
  for (auto* e : {&j["state"]["evaluation"], &j["snapshot"]["evaluation"]}) {
    for (const auto* key : {"floor_locked", "day_open_realised", "qualifying_days", "cycle_started", "payouts"}) e->erase(key);
    for (auto& day : (*e)["days"]) { day.erase("realised"); day.erase("qualifying"); }
  }
  for (const auto* key : {"phase", "lock_balance", "payouts"}) j["state"]["config"]["rules"].erase(key);
  return j.dump();
}

TEST(TradingFunded, JournalsFromBeforePayoutsRecoverAsEvaluations) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-funded-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "earlier.jsonl").string();
  ScriptedMarket f;
  auto capture = std::make_shared<CapturingJournal>();
  AccountRules rules;
  rules.plan = "Earlier";
  rules.max_drawdown = m("1000");
  {
    TradingSession s(config(rules), f.time, capture);
    f.seed(s);
    winning_trade(s, f);
    next_day(s, f, {2026, 9, 23});
  }
  {
    auto file = FileJournal::create(path);
    for (const auto& entry : capture->entries) file->append(entry.time, entry.type, before_payouts(entry.payload));
  }
  auto s = TradingSession::recover(FileJournal::read(path), FileJournal::resume(path));
  EXPECT_EQ(s.config().rules.phase, Phase::Evaluation);
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
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace openport::trading

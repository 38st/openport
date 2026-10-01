#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "openport/md/time.hpp"
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
AccountRules plan(std::string_view target = "0", std::string_view max_drawdown = "0") {
  AccountRules r;
  r.plan = "Plan rules test";
  r.profit_target = m(target);
  r.max_drawdown = m(max_drawdown);
  return r;
}
void quote(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask) {
  f.next();
  s.on_quotes({f.quote(bid, ask)}, {f.valuation()}, f.time);
}
/// Rolls over to `day` at 10:00 on the marks the old day closed on, then quotes the new day.
void next_day(TradingSession& s, ScriptedMarket& f, md::Date day, std::string_view bid = "4.00", std::string_view ask = "4.20") {
  f.time = md::new_york_to_utc(day, 10, 0);
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  quote(s, f, bid, ask);
}
class CapturingJournal final : public Journal {
 public:
  std::vector<std::string> payloads;
  void append(Timestamp, std::string_view, std::string_view payload) override { payloads.emplace_back(payload); }
  std::uint64_t sequence() const override { return payloads.size(); }
  std::string head() const override { return std::string(64, '0'); }
};
/// A journal file in a fresh temporary directory, removed afterwards.
struct JournalFile {
  std::filesystem::path directory;
  std::string path;
  JournalFile() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-plan-rules-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    directory = pattern;
    path = (directory / "account.jsonl").string();
  }
  ~JournalFile() { std::filesystem::remove_all(directory); }
};
const Objective* objective(const std::vector<Objective>& list, Reason code) {
  for (const auto& o : list) if (o.code == code) return &o;
  return nullptr;
}

TEST(PlanRules, TheTradingDayEndsAtThePlansOwnTime) {
  // 17:30 New York time on a Tuesday counts toward Wednesday under the default
  // 17:00 end, but still toward Tuesday when the plan's day ends at 18:00.
  const auto evening = md::new_york_to_utc({2026, 9, 22}, 17, 30);
  EXPECT_EQ(md::trading_date(evening), (md::Date{2026, 9, 23}));
  EXPECT_EQ(md::trading_date(evening, 17 * 60), md::trading_date(evening));
  EXPECT_EQ(md::trading_date(evening, 18 * 60), (md::Date{2026, 9, 22}));
  EXPECT_EQ(md::trading_date(md::new_york_to_utc({2026, 9, 22}, 18, 0), 18 * 60), (md::Date{2026, 9, 23}));
  // A day that ends at midnight keeps the evening; a Saturday belongs to Monday.
  EXPECT_EQ(md::trading_date(md::new_york_to_utc({2026, 9, 22}, 23, 59), 24 * 60), (md::Date{2026, 9, 22}));
  EXPECT_EQ(md::trading_date(md::new_york_to_utc({2026, 9, 26}, 12, 0), 24 * 60), (md::Date{2026, 9, 28}));

  ScriptedMarket f;
  auto rules = plan("0", "100");
  rules.day_end_minutes = 18 * 60;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  EXPECT_EQ(s.trading_date(evening), (md::Date{2026, 9, 22}));
  // At 17:30 the plan's day has not ended, so there is nothing to roll over.
  EXPECT_EQ(s.roll_day(evening).decision.code, Reason::INVALID_TIME);
  const auto later = md::new_york_to_utc({2026, 9, 22}, 18, 5);
  EXPECT_EQ(s.trading_date(later), (md::Date{2026, 9, 23}));
  ASSERT_TRUE(s.roll_day(later).decision.ok());
  EXPECT_EQ(s.trading_day(), (md::Date{2026, 9, 23}));
  ASSERT_EQ(s.snapshot()->evaluation.days.size(), 1U);
  EXPECT_EQ(s.snapshot()->evaluation.days[0].day, (md::Date{2026, 9, 22}));

  for (const std::int64_t minutes : {std::int64_t{16 * 60 + 14}, std::int64_t{24 * 60 + 1}, std::int64_t{0}}) {
    rules.day_end_minutes = minutes;
    EXPECT_THROW(validate_rules(rules), TradingError) << minutes;
  }
  rules.day_end_minutes = 16 * 60 + 15;
  EXPECT_NO_THROW(validate_rules(rules));
  rules.day_end_minutes = 24 * 60;
  EXPECT_NO_THROW(validate_rules(rules));
}

TEST(PlanRules, AStaticFloorNeverMovesAndALockAtStartStopsAtTheStartingBalance) {
  ScriptedMarket f;
  auto rules = plan("0", "100");
  rules.drawdown_mode = DrawdownMode::Static;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("9900"));
  EXPECT_TRUE(s.snapshot()->evaluation.floor_locked);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.80", "5.00");  // equity 10346.75: the peak follows, the floor does not
  EXPECT_EQ(s.snapshot()->evaluation.peak, m("10346.75"));
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("9900"));
  quote(s, f, "4.00", "4.20");
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);  // 9946.75 > 9900
  quote(s, f, "3.90", "4.00");  // 9896.75 touches 9900
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::DRAWDOWN_FLOOR);

  ScriptedMarket g;
  auto locking = plan("0", "100");
  locking.lock_at_start = true;
  TradingSession t(config(locking), g.time);
  g.seed(t);
  EXPECT_EQ(t.snapshot()->evaluation.floor, m("9900"));
  EXPECT_FALSE(t.snapshot()->evaluation.floor_locked);
  ASSERT_TRUE(t.submit(g.market("open", 5), g.time).decision.ok());
  quote(t, g, "4.20", "4.40");  // 10046.75: the floor trails to 9946.75
  EXPECT_EQ(t.snapshot()->evaluation.floor, m("9946.75"));
  quote(t, g, "4.60", "4.80");  // 10246.75: peak - 100 passes the start, so it locks there
  EXPECT_EQ(t.snapshot()->evaluation.floor, m("10000"));
  EXPECT_TRUE(t.snapshot()->evaluation.floor_locked);
  quote(t, g, "4.90", "5.10");
  EXPECT_EQ(t.snapshot()->evaluation.floor, m("10000"));

  auto invalid = plan("0", "100");
  invalid.drawdown_mode = DrawdownMode::Static;
  invalid.lock_at_start = true;
  EXPECT_THROW(validate_rules(invalid), TradingError);
  invalid.lock_at_start = false;
  invalid.lock_balance = m("9950");
  EXPECT_THROW(validate_rules(invalid), TradingError);
  invalid.drawdown_mode = DrawdownMode::Intraday;
  invalid.lock_at_start = true;
  EXPECT_THROW(validate_rules(invalid), TradingError);
}

TEST(PlanRules, ADailyLossLimitFlattensAndLocksTheDayWithoutFailing) {
  ScriptedMarket f;
  auto rules = plan("1000", "1000");
  rules.daily_loss_limit = m("200");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("resting", 1, "3.50"), f.time).decision.ok());
  quote(s, f, "3.70", "3.90");  // 7896.75 + 5 * 100 * 3.80 = 9796.75, at or below 9800
  auto snap = s.snapshot();
  auto e = snap->evaluation;
  EXPECT_EQ(e.status, EvaluationStatus::Active);
  EXPECT_EQ(e.day_lock, Reason::DAILY_LOSS_LIMIT);
  EXPECT_EQ(e.day_locked_at, f.time);
  EXPECT_EQ(snap->recent_orders[1].status, OrderStatus::Cancelled);
  EXPECT_EQ(snap->recent_orders[1].reason.code, Reason::DAILY_LOSS_LIMIT);
  const auto& close = snap->recent_orders.back();
  EXPECT_TRUE(close.system);
  EXPECT_TRUE(close.request.client_order_id.starts_with("system:daily_loss:")) << close.request.client_order_id;
  EXPECT_TRUE(snap->positions.empty());
  // Opening orders wait for the next trading day; the attempt goes on.
  EXPECT_EQ(s.submit(f.market("again"), f.time).decision.code, Reason::DAILY_LOSS_LIMIT);
  next_day(s, f, {2026, 9, 23});
  e = s.snapshot()->evaluation;
  EXPECT_EQ(e.day_lock, Reason::NONE);
  ASSERT_EQ(e.days.size(), 1U);
  EXPECT_EQ(e.days[0].locked, Reason::DAILY_LOSS_LIMIT);
  EXPECT_TRUE(s.submit(f.market("next-day"), f.time).decision.ok());
}

TEST(PlanRules, ADailyLossLimitCanFailTheAttemptAndRecoversWithItsCode) {
  ScriptedMarket f;
  auto rules = plan("1000", "1000");
  rules.daily_loss_limit = m("200");
  rules.daily_loss_action = BreachAction::Fail;
  JournalFile file;
  TradingSession s(config(rules), f.time, FileJournal::create(file.path));
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "3.70", "3.90");
  const auto e = s.snapshot()->evaluation;
  EXPECT_EQ(e.status, EvaluationStatus::Failed);
  EXPECT_EQ(e.decision_code, Reason::DAILY_LOSS_LIMIT);
  EXPECT_NE(e.decision.find("daily loss limit at $9800.00 ($200.00 below the day's opening equity, $10000.00)"), std::string::npos)
      << e.decision;
  EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:daily_loss:"));
  // The decision's code is journaled with it, and recovery restores it.
  const auto recovery = FileJournal::read(file.path);
  const auto failed = nlohmann::json::parse(recovery.records.back().payload);
  bool coded = false;
  for (const auto& event : failed.at("events"))
    if (event.at("type") == "evaluation_failed") coded = event.at("payload").at("code") == "DAILY_LOSS_LIMIT";
  EXPECT_TRUE(coded) << failed.dump();
  const auto restored = TradingSession::recover(recovery);
  EXPECT_EQ(restored.snapshot()->evaluation.decision_code, Reason::DAILY_LOSS_LIMIT);
  EXPECT_EQ(restored.snapshot_json(), s.snapshot_json());
}

TEST(PlanRules, DailyLossBasesMeasureFromBalanceEquityTheHigherOrTheDaysHigh) {
  // The day's high: a gain then a give-back reaches a limit trailing it.
  for (const auto basis : {DailyLossBasis::Equity, DailyLossBasis::Peak}) {
    ScriptedMarket f;
    auto rules = plan();
    rules.daily_loss_limit = m("150");
    rules.daily_loss_basis = basis;
    TradingSession s(config(rules), f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
    quote(s, f, "4.40", "4.60");  // 10146.75
    quote(s, f, "4.00", "4.20");  // 9946.75: 200 off the high, 53.25 off the open
    EXPECT_EQ(s.snapshot()->evaluation.day_lock, basis == DailyLossBasis::Peak ? Reason::DAILY_LOSS_LIMIT : Reason::NONE);
  }
  // Held overnight, a winner opens the next day with equity above the balance.
  for (const auto basis : {DailyLossBasis::Equity, DailyLossBasis::Balance, DailyLossBasis::Higher}) {
    ScriptedMarket f;
    auto rules = plan();
    rules.daily_loss_limit = m("100");
    rules.daily_loss_basis = basis;
    TradingSession s(config(rules), f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
    quote(s, f, "4.40", "4.60");  // closes the day at 10146.75 with a 9996.75 balance
    next_day(s, f, {2026, 9, 23}, "4.40", "4.60");
    const auto level = daily_loss_level(s.snapshot()->evaluation, rules, plan_inputs(*s.snapshot()));
    ASSERT_TRUE(level.has_value());
    EXPECT_EQ(level->reference, basis == DailyLossBasis::Balance ? m("9996.75") : m("10146.75"));
    quote(s, f, "4.10", "4.30");  // 9996.75
    EXPECT_EQ(s.snapshot()->evaluation.day_lock, basis == DailyLossBasis::Balance ? Reason::NONE : Reason::DAILY_LOSS_LIMIT);
  }
}

TEST(PlanRules, MinimumTradingDaysHoldAPassUntilEnoughDaysTraded) {
  ScriptedMarket f;
  auto rules = plan("100", "1000");
  rules.min_trading_days = 2;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");  // 10146.75 reaches the target on the first day
  auto snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Active);
  EXPECT_EQ(snap->evaluation.day_executions, 1U);
  auto objectives = evaluation_objectives(snap->evaluation, rules, plan_inputs(*snap));
  ASSERT_NE(objective(objectives, Reason::PROFIT_TARGET), nullptr);
  EXPECT_TRUE(objective(objectives, Reason::PROFIT_TARGET)->met);
  ASSERT_NE(objective(objectives, Reason::MIN_TRADING_DAYS), nullptr);
  EXPECT_FALSE(objective(objectives, Reason::MIN_TRADING_DAYS)->met);
  EXPECT_EQ(objective(objectives, Reason::MIN_TRADING_DAYS)->actual, 1.0);
  // Holding into the next day is not trading on it.
  next_day(s, f, {2026, 9, 23}, "4.40", "4.60");
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  EXPECT_EQ(s.snapshot()->evaluation.days.at(0).executions, 1U);
  // A trade on the second day passes at once.
  ASSERT_TRUE(s.submit(f.market("second", 1, Side::Sell), f.time).decision.ok());
  snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Passed);
  EXPECT_EQ(snap->evaluation.decision_code, Reason::PROFIT_TARGET);
}

TEST(PlanRules, MinimumProfitableDaysCountDaysAboveTheThreshold) {
  ScriptedMarket f;
  auto rules = plan("100", "1000");
  rules.min_profitable_days = 2;
  rules.profitable_day_profit = m("100");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");  // +146.75 today
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  next_day(s, f, {2026, 9, 23}, "4.40", "4.60");
  quote(s, f, "4.50", "4.70");  // +50 today: not profitable enough
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  quote(s, f, "4.60", "4.80");  // +100 today
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
}

TEST(PlanRules, AConsistencyRuleNeedsTheBestDayWithinItsShareOfTheTotal) {
  ScriptedMarket f;
  auto rules = plan("100", "1000");
  rules.consistency_percent = 50;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");  // one day of +146.75 is all of the profit
  auto snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Active);
  auto in = plan_inputs(*snap);
  const auto objectives = evaluation_objectives(snap->evaluation, rules, in);
  const auto* consistency = objective(objectives, Reason::CONSISTENCY);
  ASSERT_NE(consistency, nullptr);
  EXPECT_FALSE(consistency->met);
  EXPECT_EQ(consistency->actual, 100.0);
  EXPECT_EQ(consistency_target(snap->evaluation, rules, in), m("293.50"));
  next_day(s, f, {2026, 9, 23}, "4.40", "4.60");
  quote(s, f, "4.60", "4.80");  // +100: total 246.75, best day 146.75 is 59%
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  quote(s, f, "4.70", "4.90");  // +150 today is the best day now: 150 / 296.75 is 50.5%
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  quote(s, f, "4.68", "4.88");  // +140 today: 146.75 / 286.75 is 51%
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  next_day(s, f, {2026, 9, 24}, "4.68", "4.88");
  quote(s, f, "4.72", "4.92");  // +20: 146.75 / 306.75 is 47.8%
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);

  // Measured against the profitable days alone, losing days do not dilute it.
  auto positive = rules;
  positive.consistency_basis = ConsistencyBasis::PositiveDays;
  Evaluation e;
  e.started = 1;
  e.starting_balance = m("10000");
  e.days.push_back({{2026, 9, 22}, m("10000"), m("10300"), {}, {}, {}, false, {}, {}, {}, 0, 0, 0, Reason::NONE});
  e.days.push_back({{2026, 9, 23}, m("10300"), m("10100"), {}, {}, {}, false, {}, {}, {}, 0, 0, 0, Reason::NONE});
  e.day_open_equity = m("10100");
  const PlanInputs now{m("10400"), m("10400"), {}, true};
  EXPECT_FALSE(objective(evaluation_objectives(e, rules, now), Reason::CONSISTENCY)->met);   // 300 of 400
  EXPECT_TRUE(objective(evaluation_objectives(e, positive, now), Reason::CONSISTENCY)->met); // 300 of 600
}

TEST(PlanRules, ATargetOnTheClosedBalanceNeedsTheTradeClosed) {
  ScriptedMarket f;
  auto rules = plan("100", "1000");
  rules.profit_basis = ProfitBasis::Balance;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  auto snap = s.snapshot();
  // Closing now sells at the 4.00 bid and pays the fees again.
  EXPECT_EQ(snap->equity, m("9946.75"));
  EXPECT_EQ(snap->exit_equity, m("9893.50"));
  quote(s, f, "4.60", "4.80");  // equity 10246.75 is above the target, the balance is not
  snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Active);
  EXPECT_EQ(plan_inputs(*snap).balance, m("9996.75"));
  ASSERT_TRUE(s.submit(f.market("close", 5, Side::Sell), f.time).decision.ok());
  snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Passed);
  EXPECT_EQ(snap->evaluation.decided_equity, m("10193.50"));
  EXPECT_EQ(snap->evaluation.decision, "Balance $10193.50 reached the profit target $10100.00 with every position closed");
}

TEST(PlanRules, RulesThatAreOffKeepTheJournalUnchanged) {
  ScriptedMarket f;
  auto journal = std::make_shared<CapturingJournal>();
  TradingSession s(config(plan("100", "1000")), f.time, journal);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::PROFIT_TARGET);
  for (const auto& payload : journal->payloads) {
    for (const auto* key : {"decision_code", "day_executions", "day_lock", "daily_loss_limit", "consistency_percent",
                            "day_end_minutes", "profit_basis", "lock_at_start", "executions"})
      EXPECT_EQ(payload.find(key), std::string::npos) << key;
    const auto record = nlohmann::json::parse(payload);
    for (const auto& event : record.at("events"))
      if (event.at("type") == "evaluation_passed") { EXPECT_FALSE(event.at("payload").contains("code")); }
  }
  // The rules round-trip through the journal, each recorded only when set.
  auto rules = plan("100", "1000");
  rules.lock_at_start = true;
  rules.profit_basis = ProfitBasis::Balance;
  rules.daily_loss_limit = m("50");
  rules.daily_loss_basis = DailyLossBasis::Higher;
  rules.daily_loss_action = BreachAction::Fail;
  rules.consistency_percent = 40;
  rules.consistency_basis = ConsistencyBasis::PositiveDays;
  rules.min_trading_days = 3;
  rules.min_profitable_days = 2;
  rules.profitable_day_profit = m("25");
  rules.day_end_minutes = 18 * 60;
  JournalFile file;
  { TradingSession t(config(rules), f.time, FileJournal::create(file.path)); }
  const auto recovery = FileJournal::read(file.path);
  const auto recorded = nlohmann::json::parse(recovery.records.at(0).payload).at("state").at("config").at("rules");
  EXPECT_EQ(recorded.at("daily_loss_basis"), "higher");
  EXPECT_EQ(recorded.at("day_end_minutes"), 18 * 60);
  EXPECT_EQ(TradingSession::recover(recovery).config().rules, rules);
}

TEST(PlanRules, PassOddsHonourTheMinimumDays) {
  ScriptedMarket f;
  auto rules = plan("100", "1000");
  Evaluation e;
  e.started = f.time;
  e.starting_balance = m("10000");
  e.peak = e.starting_balance;
  e.floor = m("9000");
  e.day = {2026, 9, 22};
  e.day_open_equity = e.starting_balance;
  std::vector<EvaluationDay> history;
  for (int i = 0; i < 12; ++i) {
    EvaluationDay day;
    day.day = md::date_from_days(md::days_since_epoch({2026, 8, 1}) + i);
    day.open_equity = m("10000");
    day.close_equity = m("10060");
    day.low_equity = m("9990");
    day.high_equity = m("10070");
    day.low_at = 1;
    day.high_at = 2;
    history.push_back(day);
  }
  const auto free = pass_odds(e, rules, m("10000"), history, 10, 100, 7);
  EXPECT_EQ(free.pass, 1.0);
  EXPECT_EQ(free.median_days_to_pass, 2.0);
  rules.min_trading_days = 5;
  const auto held = pass_odds(e, rules, m("10000"), history, 10, 100, 7);
  EXPECT_EQ(held.pass, 1.0);
  EXPECT_EQ(held.median_days_to_pass, 5.0);
  rules.min_trading_days = 20;
  EXPECT_EQ(pass_odds(e, rules, m("10000"), history, 10, 100, 7).pass, 0.0);
}

}  // namespace
}  // namespace openport::trading

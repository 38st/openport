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
AccountRules plan() {
  AccountRules r;
  r.plan = "Time rules test";
  return r;
}
struct JournalFile {
  std::filesystem::path directory;
  std::string path;
  JournalFile() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-time-rules-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    directory = pattern;
    path = (directory / "account.jsonl").string();
  }
  ~JournalFile() { std::filesystem::remove_all(directory); }
};
TEST(TimeRules, LegacyPlanJournalBytes) {
  ScriptedMarket f;
  auto rules = plan();
  rules.profit_target = m("100");
  rules.max_drawdown = m("1000");
  JournalFile file;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
    f.next();
    s.on_quotes({f.quote("4.40", "4.60")}, {f.valuation()}, f.time);
    EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  }
  const auto recovery = FileJournal::read(file.path);
  // Recorded by the pre-time-rules reducer at 85e64e20 with this scripted market.
  EXPECT_EQ(recovery.head, "4e40378560db4e891c76cb27a686dfd2baf9c5a98dbae7ab4590dbd30d57acb0");
  // Default-off fields are absent in every checkpoint, delta and event.
  for (const auto& record : recovery.records)
    for (const auto* key : {"time_limit_days", "inactivity_days", "underlyings", "trading_start", "trading_end", "last_activity"})
      EXPECT_EQ(record.payload.find(std::string("\"") + key + "\""), std::string::npos) << key;
  // Explicitly disabling each field takes exactly the legacy command path.
  rules.time_limit_days = 0; rules.inactivity_days = 0; rules.underlyings.clear();
  rules.trading_start.reset(); rules.trading_end.reset();
  ScriptedMarket g;
  JournalFile again;
  {
    TradingSession s(config(rules), g.time, FileJournal::create(again.path));
    g.seed(s);
    ASSERT_TRUE(s.submit(g.market("open", 5), g.time).decision.ok());
    g.next(); s.on_quotes({g.quote("4.40", "4.60")}, {g.valuation()}, g.time);
  }
  const auto repeated = FileJournal::read(again.path);
  ASSERT_EQ(recovery.records.size(), repeated.records.size());
  for (std::size_t i = 0; i < recovery.records.size(); ++i) EXPECT_EQ(recovery.records[i].payload, repeated.records[i].payload);
  EXPECT_EQ(recovery.head, repeated.head);
}

void next_day(TradingSession& s, ScriptedMarket& f, md::Date day) {
  f.time = md::new_york_to_utc(day, 10, 0);
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  ++f.observation;
  s.on_quotes({f.quote()}, {f.valuation()}, f.time);
}
TEST(TimeRules, TimeLimitFailsAfterTheDeadlineOnAnEmptyBatch) {
  ScriptedMarket f;
  auto rules = plan();
  rules.time_limit_days = 2;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  next_day(s, f, {2026, 9, 24});
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  const auto progress = time_rule_progress(s.snapshot()->evaluation, rules, f.time);
  EXPECT_EQ(progress.deadline, (md::Date{2026, 9, 24}));
  EXPECT_EQ(progress.days_left, 0);
  // 17:00 belongs to Friday under this plan; even a flat empty batch decides.
  f.time = md::new_york_to_utc({2026, 9, 24}, 17, 0);
  s.on_quotes({}, {}, f.time);
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::TIME_LIMIT);
  EXPECT_EQ(s.snapshot()->evaluation.decided_at, f.time);
  EXPECT_EQ(s.snapshot()->evaluation.decision, "The evaluation's 2-day window ended on 2026-09-24");
  EXPECT_EQ(s.submit(f.market("late"), f.time).decision.code, Reason::EVALUATION_CLOSED);
}
TEST(TimeRules, PassBeforeDeadlineStandsAndZeroStartWaitsForMarketTime) {
  ScriptedMarket f;
  auto rules = plan(); rules.time_limit_days = 1; rules.profit_target = m("100");
  TradingSession s(config(rules), 0);
  EXPECT_FALSE(time_rule_progress(s.snapshot()->evaluation, rules, 0).deadline);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  f.next(); s.on_quotes({f.quote("4.40", "4.60")}, {f.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::PROFIT_TARGET);
}
TEST(TimeRules, OwnExecutionsResetInactivityButOrdersDoNot) {
  ScriptedMarket f;
  auto rules = plan(); rules.inactivity_days = 2;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  EXPECT_EQ(time_rule_progress(s.snapshot()->evaluation, rules, f.time).last_activity, f.time);
  next_day(s, f, {2026, 9, 23});
  ASSERT_TRUE(s.submit(f.market("own"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.last_activity, f.time);
  const auto activity = f.time;
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  ASSERT_TRUE(s.submit(f.limit("working", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.last_activity, activity);
  // A late execution cannot revive it; the time check runs before matching.
  f.time = md::new_york_to_utc({2026, 9, 28}, 10, 0);
  EXPECT_EQ(s.submit(f.market("too-late"), f.time).decision.code, Reason::EVALUATION_CLOSED);
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::INACTIVITY);
  EXPECT_EQ(s.snapshot()->evaluation.last_activity, activity);
}
TEST(TimeRules, SystemLiquidationIsNotActivityAndFundedAccountsCanExpire) {
  ScriptedMarket f;
  auto rules = plan(); rules.inactivity_days = 2; rules.daily_loss_limit = m("100");
  rules.phase = Phase::Funded; rules.payouts.qualifying_days = 1;
  TradingSession s(config(rules), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("own", 5), f.time).decision.ok());
  const auto activity = f.time;
  next_day(s, f, {2026, 9, 23});
  f.next(); s.on_quotes({f.quote("3.50", "3.70")}, {f.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->evaluation.day_lock, Reason::DAILY_LOSS_LIMIT);
  ASSERT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->evaluation.last_activity, activity);
  next_day(s, f, {2026, 9, 24});
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::INACTIVITY);
}
TEST(TimeRules, ShareExecutionsCountAsOwnActivity) {
  ScriptedMarket f;
  auto rules = plan(); rules.inactivity_days = 2;
  TradingSession s(config(rules), f.time); f.seed(s);
  next_day(s, f, {2026, 9, 23});
  ASSERT_TRUE(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.last_activity, f.time);
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
}
TEST(TimeRules, WhitelistUsesUnderlyingForOptionsAndShares) {
  ScriptedMarket f;
  auto rules = plan(); rules.underlyings = {"SPX", "SPY"};
  TradingSession s(config(rules), f.time); f.seed(s);
  // SPXW is admitted under SPX, not its option root.
  ASSERT_TRUE(s.submit(f.market("spxw"), f.time).decision.ok());
  ASSERT_TRUE(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
  const auto shares = s.trade_stock("QQQ", 1, f.time, StockPrice{"QQQ", f.time, m("450")}).decision;
  EXPECT_EQ(shares.code, Reason::INSTRUMENT_NOT_ALLOWED); EXPECT_EQ(shares.scope, "QQQ");
  ScriptedMarket g; g.contract = *md::parse_osi("XSP261022C00500000"); g.seed(s);
  const auto denied = s.submit(g.market("xsp"), f.time).decision;
  EXPECT_EQ(denied.code, Reason::INSTRUMENT_NOT_ALLOWED); EXPECT_EQ(denied.scope, "XSP");
}
TEST(TimeRules, TradingHoursCancelOpeningsAtEndButKeepReducingOrdersAndExits) {
  ScriptedMarket f;
  auto rules = plan(); rules.trading_start = 10 * 60; rules.trading_end = 11 * 60;
  TradingSession s(config(rules), f.time); f.seed(s);
  auto entry = f.market("position", 2);
  entry.bracket = Bracket{{}, ExitSpec{{}, m("8")}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  const auto opening = s.submit(f.limit("opening", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time);
  const auto reducing = s.submit(f.limit("reducing", 1, "4.80", Side::Sell, TimeInForce::Gtc), f.time);
  ASSERT_TRUE(opening.decision.ok()); ASSERT_TRUE(reducing.decision.ok());
  f.time = md::new_york_to_utc({2026, 9, 22}, 10, 59);
  s.on_quotes({}, {}, f.time);
  EXPECT_TRUE(s.snapshot()->recent_orders.at(*opening.order_id - 1).open());
  f.time += md::kNanosPerMinute;
  s.on_quotes({}, {}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders.at(*opening.order_id - 1).reason.code, Reason::OUTSIDE_PLAN_HOURS);
  EXPECT_EQ(s.snapshot()->recent_orders.at(*opening.order_id - 1).reason.actual, 660); EXPECT_EQ(s.snapshot()->recent_orders.at(*opening.order_id - 1).reason.limit, 660);
  EXPECT_TRUE(s.snapshot()->recent_orders.at(*reducing.order_id - 1).open());
  EXPECT_TRUE(s.snapshot()->recent_orders.at(1).open()); // managed take-profit
  ++f.observation; s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  EXPECT_EQ(s.submit(f.market("outside"), f.time).decision.code, Reason::OUTSIDE_PLAN_HOURS);
  ASSERT_TRUE(s.cancel(*reducing.order_id, f.time).decision.ok());
  EXPECT_TRUE(s.submit(f.market("close", 2, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
}
TEST(TimeRules, HoursApplyBeforeStartInCurbAndAfterDaylightSaving) {
  for (const md::Date date : {md::Date{2026, 9, 22}, md::Date{2026, 11, 3}}) {
    ScriptedMarket f; f.contract = *md::parse_osi("SPXW261119C05000000");
    f.time = md::new_york_to_utc(date, 9, 30);
    auto rules = plan(); rules.trading_start = 9 * 60 + 31; rules.trading_end = 16 * 60;
    TradingSession s(config(rules), f.time); f.seed(s);
    const auto early = s.submit(f.market("early"), f.time).decision;
    EXPECT_EQ(early.code, Reason::OUTSIDE_PLAN_HOURS); EXPECT_EQ(early.actual, 570); EXPECT_EQ(early.limit, 571);
    f.time += md::kNanosPerMinute; ++f.observation; s.on_quotes({f.quote()}, {f.valuation()}, f.time);
    EXPECT_TRUE(s.submit(f.market("at-start"), f.time).decision.ok());
    f.time = md::new_york_to_utc(date, 16, 30);
    auto curb = f.limit("curb"); curb.tif = TimeInForce::GtcExto;
    EXPECT_EQ(s.submit(curb, f.time).decision.code, Reason::OUTSIDE_PLAN_HOURS);
    f.time = md::new_york_to_utc(date, 20, 30);
    curb.client_order_id = "global";
    EXPECT_EQ(s.submit(curb, f.time).decision.code, Reason::OUTSIDE_PLAN_HOURS);
  }
}
TEST(TimeRules, RulesAndActivityRecoverAndTheRecoveredDeadlineStillFails) {
  ScriptedMarket f;
  auto rules = plan(); rules.time_limit_days = 10; rules.inactivity_days = 2;
  rules.underlyings = {"SPX"}; rules.trading_start = 9 * 60 + 30; rules.trading_end = 16 * 60;
  JournalFile file;
  std::string snapshot;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(file.path)); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("own"), f.time).decision.ok());
    snapshot = s.snapshot_json();
  }
  const auto recovery = FileJournal::read(file.path);
  auto s = TradingSession::recover(recovery, FileJournal::resume(file.path));
  EXPECT_EQ(s.config().rules, rules); EXPECT_EQ(s.snapshot_json(), snapshot);
  EXPECT_EQ(s.snapshot()->evaluation.last_activity, f.time);
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::INACTIVITY);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
}
TEST(TimeRules, ValidationAndCalendarDaysFollowThePlanBoundary) {
  for (const auto n : {-1, 367}) {
    auto rules = plan(); rules.time_limit_days = n; EXPECT_THROW(validate_rules(rules), TradingError);
    rules.time_limit_days = 0; rules.inactivity_days = n; EXPECT_THROW(validate_rules(rules), TradingError);
  }
  auto rules = plan(); rules.phase = Phase::Funded; rules.payouts.qualifying_days = 1; rules.time_limit_days = 1;
  EXPECT_THROW(validate_rules(rules), TradingError);
  rules = plan(); rules.trading_start = 0; EXPECT_THROW(validate_rules(rules), TradingError);
  rules.trading_end = 1440; EXPECT_NO_THROW(validate_rules(rules));
  rules.trading_start = 1440; EXPECT_THROW(validate_rules(rules), TradingError);
  for (const auto& symbols : {std::vector<std::string>{"SPX", "SPX"}, std::vector<std::string>{"spx"}, std::vector<std::string>{""}, std::vector<std::string>(33, "SPX")}) {
    rules = plan(); rules.underlyings = symbols; EXPECT_THROW(validate_rules(rules), TradingError);
  }
  ScriptedMarket f; f.time = md::new_york_to_utc({2026, 10, 30}, 17, 30);
  rules = plan(); rules.time_limit_days = 3; rules.day_end_minutes = 18 * 60;
  TradingSession s(config(rules), f.time);
  auto p = time_rule_progress(s.snapshot()->evaluation, rules, md::new_york_to_utc({2026, 11, 2}, 10, 0));
  EXPECT_EQ(p.deadline, (md::Date{2026, 11, 2})); EXPECT_EQ(p.days_left, 0);
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 11, 2}, 17, 59));
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 11, 2}, 18, 0));
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::TIME_LIMIT);
}

TEST(TimeRules, RecoveredPositionsOutsideWhitelistCanStillBeReduced) {
  ScriptedMarket f;
  JournalFile source, restricted;
  {
    TradingSession s(config(plan()), f.time, FileJournal::create(source.path)); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held", 2), f.time).decision.ok());
    ASSERT_TRUE(s.trade_stock("SPY", 2, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
  }
  // Import held positions under a restricted plan. Recovery preserves outcomes;
  // its next own orders must still be able to reduce every inherited holding.
  {
    const auto journal = FileJournal::create(restricted.path);
    for (const auto& record : FileJournal::read(source.path).records) {
      auto payload = nlohmann::json::parse(record.payload);
      if (payload.contains("state")) payload["state"]["config"]["rules"]["underlyings"] = {"XSP"};
      journal->append(record.time, record.type, payload.dump());
    }
  }
  auto s = TradingSession::recover(FileJournal::read(restricted.path));
  EXPECT_EQ(s.submit(f.market("add"), f.time).decision.code, Reason::INSTRUMENT_NOT_ALLOWED);
  EXPECT_TRUE(s.submit(f.market("reduce", 1, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.trade_stock("SPY", 1, f.time).decision.code, Reason::INSTRUMENT_NOT_ALLOWED);
  EXPECT_TRUE(s.trade_stock("SPY", -1, f.time).decision.ok());
  EXPECT_TRUE(s.trade_stock("SPY", -1, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty()); EXPECT_TRUE(s.snapshot()->stocks.empty());
}

TEST(TimeRules, AGapOverTheEndCancelsBeforeTheNextWindowsFirstFill) {
  ScriptedMarket f;
  auto rules = plan(); rules.trading_start = 570; rules.trading_end = 960;
  TradingSession s(config(rules), f.time); f.seed(s);
  const auto opening = s.submit(f.limit("overnight", 1, "4.00", Side::Buy, TimeInForce::Gtc), f.time);
  ASSERT_TRUE(opening.decision.ok());
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0); ++f.observation;
  s.on_quotes({f.quote("3.80", "4.00")}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders.at(*opening.order_id - 1).reason.code, Reason::OUTSIDE_PLAN_HOURS);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TimeRules, ComboEntriesRespectHoursAndClosingLegsRemainAllowed) {
  ScriptedMarket f, g; g.contract = *md::parse_osi("SPXW261022C05010000");
  auto rules = plan(); rules.underlyings = {"SPX"}; rules.trading_start = 570; rules.trading_end = 660;
  TradingSession s(config(rules), f.time); f.seed(s); g.seed(s);
  auto entry = f.market("spread"); entry.symbol.clear();
  entry.legs = {{f.symbol(), Side::Buy, 1}, {g.symbol(), Side::Sell, 1}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  f.time = g.time = md::new_york_to_utc({2026, 9, 22}, 11, 0);
  ++f.observation; ++g.observation;
  s.on_quotes({f.quote(), g.quote()}, {f.valuation(), g.valuation()}, f.time);
  entry.client_order_id = "late-spread";
  EXPECT_EQ(s.submit(entry, f.time).decision.code, Reason::OUTSIDE_PLAN_HOURS);
  entry.client_order_id = "close-spread";
  entry.legs[0].side = Side::Sell; entry.legs[1].side = Side::Buy;
  EXPECT_TRUE(s.submit(entry, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
}
TEST(TimeRules, ProjectedOddsRespectCalendarDeadlines) {
  std::vector<EvaluationDay> history;
  for (int i = 0; i < 12; ++i) {
    EvaluationDay day;
    day.day = md::date_from_days(md::days_since_epoch({2026, 8, 1}) + i);
    day.open_equity = m("10000"); day.close_equity = m("10060");
    day.low_equity = m("9990"); day.high_equity = m("10070");
    day.low_at = 1; day.high_at = 2;
    history.push_back(day);
  }
  const auto project = [&](std::int64_t limit, std::int64_t inactivity, md::Date date) {
    ScriptedMarket f; f.time = md::new_york_to_utc(date, 10, 0);
    auto rules = plan(); rules.profit_target = m("100");
    rules.time_limit_days = limit; rules.inactivity_days = inactivity;
    TradingSession s(config(rules), f.time); f.seed(s);
    return pass_odds(s.snapshot()->evaluation, rules, s.snapshot()->equity, history, 10, 100, 7);
  };
  // The sampled path needs two days to pass; the deadline day still counts.
  EXPECT_EQ(project(1, 0, {2026, 9, 22}).fail, 1.0);
  EXPECT_EQ(project(2, 0, {2026, 9, 22}).pass, 1.0);
  // The next trading date after Friday is Monday, three calendar days later.
  EXPECT_EQ(project(0, 1, {2026, 9, 25}).fail, 1.0);
  EXPECT_EQ(project(0, 3, {2026, 9, 25}).pass, 1.0);
}
}  // namespace
}  // namespace openport::trading

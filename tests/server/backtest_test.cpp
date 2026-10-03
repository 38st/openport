#include <fstream>
#include <sstream>
#include <gtest/gtest.h>
#include "openport/server/backtest.hpp"
#include "openport/server/plans.hpp"
#include "openport/server/web_policy.hpp"
#include "openport/providers/replay_batches.hpp"
#include "server/run_json.hpp"
#include "support/recording.hpp"
#include "support/contract_capture.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;
json catalogue() {
  json definition{{"id", "batch"}, {"version", 1}, {"name", "Batch fixture"}, {"description", "Simulated test prices"}, {"underlyings", {"SPX"}},
      {"window", {{"start", "09:30"}, {"end", "09:31"}, {"weekdays", {1, 2, 3, 4, 5}}}}, {"conditions", json::object()},
      {"structure", {{"template", {{"kind", "vertical"}, {"type", "put"}, {"direction", "credit"}, {"target", {{"mode", "strike"}, {"value", 5000}}}, {"width", 5}}}, {"expiry", {{"min", 0}, {"max", 1}}}}},
      {"sizing", {{"units", 1}}}, {"management", {{"close_by", "09:32"}}},
      {"guardrails", {{"max_entries_per_day", 1}, {"cooldown_minutes", 0}}}};
  return {{"schema", 1}, {"definitions", {{"batch", {{"versions", {definition}}, {"deleted", false}}}}}, {"modes", json::object()}};
}
std::string bytes(const std::filesystem::path& path) {
  std::ifstream input(path); std::ostringstream out; out << input.rdbuf(); return out.str();
}
std::filesystem::path recorded_day(const std::filesystem::path& directory, md::Date date, bool loss = false, bool breadth = false) {
  const auto path = directory / (md::format_date(date) + ".oprec");
  auto contract = *md::parse_osi("SPXW  260916P05000000");
  contract.expiry = date;
  auto lower = contract; lower.strike = 4995;
  md::RecordingHeader header;
  header.provider = "simulated test";
  header.subscription.underlyings = {"SPX", "SPY"};
  header.started = md::new_york_to_utc(date, 9, 30);
  header.capabilities.poll_interval = std::chrono::seconds(60);
  md::Timestamp time = header.started;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return time; };
  test::DiscardEvents discard;
  md::RecordingSink sink(path, header, discard, recording);
  sink.publish(md::ContractDefinition{0, contract});
  sink.publish(md::ContractDefinition{1, lower});
  if (breadth) sink.publish(md::UnderlyingClose{"SPX", time, md::previous_business_day(date), 5000});
  for (int minute = 0; minute < 3; ++minute) {
    time = header.started + minute * md::kNanosPerMinute;
    const double high = minute == 0 ? 10 : breadth && minute == 2 ? 9.5 : loss && minute == 2 ? 13 : 9;
    sink.publish(md::UnderlyingQuote{"SPX", time, 5000, 5000, 5000});
    sink.publish(md::OptionQuote{0, time, high, high + .1, 20, 20});
    sink.publish(md::OptionQuote{1, time, 8, 8.1, 20, 20});
    sink.publish(md::SnapshotComplete{"SPX", time});
  }
  sink.close();
  return path;
}
server::BacktestRequest request_for(const std::vector<std::filesystem::path>& files) {
  json days = json::array();
  for (const auto& file : files) days.push_back({{"file", file.string()}});
  auto request = server::parse_backtest({{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"days", days}}, catalogue(), {}, {}, false);
  request.analytics.fallback_rate = 0;
  return request;
}
TEST(Backtest, GapAndTrailingRulesMatchFourWorkersAndVerify) {
  test::RecordingFile storage;
  std::vector<std::filesystem::path> files;
  for (int day = 14; day <= 17; ++day) files.push_back(recorded_day(storage.directory, {2026, 9, day}, false, true));
  auto request = request_for(files);
  auto& setup = request.playbooks["definitions"]["batch"]["versions"][0];
  setup["conditions"] = {{"gap", {{"min_percent", -1}, {"max_percent", 1}}}};
  setup["management"] = {{"close_by", "15:45"}, {"trailing_stop", {{"percent", 50}}}};
  const std::atomic_bool cancel{false};
  request.workers = 1;
  const auto first = server::run_backtest(request, storage.directory / "one", cancel);
  ASSERT_EQ(first.at("status"), "completed") << first.dump();
  request.workers = 4;
  const auto second = server::run_backtest(request, storage.directory / "four", cancel);
  EXPECT_EQ(first, second);
  for (const auto& day : first.at("days")) {
    ASSERT_EQ(day.at("fills").size(), 4U) << day.dump();
    ASSERT_EQ(day.at("trades").size(), 1U);
    EXPECT_EQ(day.at("trades")[0].at("rules").at("trailing_stop"), true);
    const auto journal = day.at("journal").get<std::string>();
    EXPECT_EQ(bytes(storage.directory / "one" / journal), bytes(storage.directory / "four" / journal));
    EXPECT_NE(bytes(storage.directory / "one" / journal).find("Playbook automatic trailing stop"), std::string::npos);
    const auto verified = server::verify_run(storage.directory / "one" / journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
  for (const auto& attempt : first.at("attempts")) {
    const auto verified = server::verify_run(storage.directory / "one" / attempt.at("journal").get<std::string>());
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}
TEST(Backtest, AttemptsCarryBalancesDecidePassAndFailureAndLeaveOpenTail) {
  test::RecordingFile storage;
  std::vector<std::filesystem::path> files;
  for (const auto date : {md::Date{2026, 9, 14}, md::Date{2026, 9, 15}, md::Date{2026, 9, 16}, md::Date{2026, 9, 17}})
    files.push_back(recorded_day(storage.directory, date, date.day == 16));
  auto request = request_for(files);
  request.config.rules.profit_target = Money::parse("150");
  // Admit the spread's initial risk, then fail on a reversal from its marked high.
  request.config.rules.max_drawdown = Money::parse("350");
  request.config.rules.drawdown_mode = trading::DrawdownMode::Intraday;
  const std::atomic_bool cancel{false};
  const auto root = storage.directory / "batch";
  const auto report = server::run_backtest(request, root, cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  ASSERT_EQ(report.at("attempts").size(), 3U) << report.dump();
  EXPECT_EQ(report.at("attempts")[0].at("days"), 2);
  EXPECT_EQ(report.at("attempts")[0].at("outcome"), "passed");
  EXPECT_EQ(report.at("attempts")[1].at("outcome"), "failed");
  EXPECT_EQ(report.at("attempts")[2].at("outcome"), "open");
  EXPECT_EQ(report.at("summary").at("pass_rate"), .5);
  EXPECT_EQ(report.at("days")[0].at("pnl"), "77.40");
  EXPECT_EQ(report.at("days")[0].at("ended"), md::format_timestamp(md::new_york_to_utc({2026, 9, 14}, 9, 32)));
  EXPECT_EQ(report.at("days")[0].at("fills").size(), 4U);
  EXPECT_EQ(report.at("days")[0].at("max_drawdown"), "11.30");
  const auto daily = trading::FileJournal::read((root / "days/000001.jsonl").string());
  bool found_start = false;
  for (const auto& record : daily.records) {
    if (record.type != "run_input") continue;
    const auto payload = json::parse(record.payload);
    for (const auto& event : payload.at("events")) {
      if (event.at("type") == "run_input" && event.at("payload").at("kind") == "start") {
        found_start = true;
        EXPECT_EQ(event.at("payload").at("symbols"), json::array({"SPX", "SPY"}));
      }
    }
  }
  EXPECT_EQ(report.at("schema"), 2);
  for (const auto& attempt : report.at("attempts")) {
    auto journal = trading::FileJournal::read((root / attempt.at("journal").get<std::string>()).string());
    ASSERT_EQ(attempt.at("day_rows").size(), attempt.at("days").get<std::size_t>());
    Money previous_balance = request.config.initial_cash, previous_equity = request.config.initial_cash;
    for (const auto& row : attempt.at("day_rows")) {
      auto prefix = journal;
      const auto end = *md::parse_datetime(row.at("ended").get<std::string>(), md::Zone::Utc);
      while (!prefix.records.empty() && prefix.records.back().time > end) prefix.records.pop_back();
      prefix.head = prefix.records.back().hash;
      const auto session = trading::TradingSession::recover(prefix);
      const auto snapshot = session.snapshot();
      const auto inputs = trading::plan_inputs(*snapshot);
      EXPECT_EQ(row.at("start_balance"), previous_balance.str());
      EXPECT_EQ(row.at("start_equity"), previous_equity.str());
      EXPECT_EQ(row.at("end_balance"), inputs.balance.str());
      EXPECT_EQ(row.at("end_equity"), snapshot->equity.str());
      EXPECT_EQ(row.at("pnl"), (snapshot->equity - previous_equity).str());
      EXPECT_EQ(row.at("floor"), snapshot->evaluation.floor.str());
      EXPECT_EQ(row.at("floor_distance"), (snapshot->equity - snapshot->evaluation.floor).str());
      EXPECT_EQ(row.at("peak"), snapshot->evaluation.peak.str());
      EXPECT_EQ(row.at("target_progress"), trading::attempt_profit(snapshot->evaluation, request.config.rules, inputs).str());
      EXPECT_EQ(row.at("day_lock"), trading::to_string(snapshot->evaluation.day_lock));
      EXPECT_EQ(row.at("decision_code"), trading::to_string(snapshot->evaluation.decision_code));
      EXPECT_EQ(row.at("trades_opened"), 1);
      previous_balance = inputs.balance; previous_equity = snapshot->equity;
    }
  }
  EXPECT_TRUE(found_start);
  EXPECT_EQ(report.at("days")[0].at("trades").size(), 1U);
  EXPECT_EQ(report.at("summary").at("worst_days")[0], 2);
  EXPECT_FALSE(report.at("days")[2].at("rule_trips").empty());
  const json state{{"id", "000099"}, {"status", "completed"}, {"phase", "finished"}, {"completed", 8}, {"total", 8},
      {"label", server::kBacktestLabel}, {"directory", root.string()}, {"report", report}};
  test::capture_contract("backtests", "GET", "/api/backtests/000099", {200, state.dump()});
  for (const auto& attempt : report.at("attempts")) {
    const auto verified = server::verify_run(root / attempt.at("journal").get<std::string>());
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}
TEST(Backtest, EndOfDayFloorsRatchetAcrossRealAccountDays) {
  test::RecordingFile storage;
  auto request = request_for({recorded_day(storage.directory, {2026, 9, 14}), recorded_day(storage.directory, {2026, 9, 15})});
  request.config.rules.profit_target = Money::parse("1000");
  request.config.rules.max_drawdown = Money::parse("500");
  const std::atomic_bool cancel{false};
  const auto root = storage.directory / "eod";
  const auto report = server::run_backtest(request, root, cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  ASSERT_EQ(report.at("attempts").size(), 1U);
  EXPECT_EQ(report.at("summary").at("pass_rate"), nullptr);
  const auto recovery = trading::TradingSession::recover(trading::FileJournal::read((root / "attempts/000001.jsonl").string()));
  EXPECT_EQ(recovery.snapshot()->evaluation.peak, request.config.initial_cash + Money::parse("77.40"));
  EXPECT_EQ(recovery.snapshot()->evaluation.floor, request.config.initial_cash + Money::parse("77.40") - Money::parse("500"));
  EXPECT_EQ(report.at("attempts")[0].at("pnl"), "154.80");
}
TEST(Backtest, EntryReasonsGiveWhatBlockedEntriesInsideTheWindow) {
  // B47: the reasons were read at the end of the input, after every window had closed,
  // so each day said only "Outside entry window".
  test::RecordingFile storage;
  auto request = request_for({recorded_day(storage.directory, {2026, 9, 14})});
  // A dollar of floor room fits no spread.
  request.config.rules.max_drawdown = Money::parse("1");
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "reasons", cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  const auto& day = report.at("days")[0];
  EXPECT_TRUE(day.at("fills").empty());
  EXPECT_EQ(day.at("ended"), md::format_timestamp(md::new_york_to_utc({2026, 9, 14}, 9, 32)));
  EXPECT_EQ(day.at("entry_reasons"), json({{"batch:SPX", "No units fit buying power, limits and floor room"}})) << day.dump();
  EXPECT_EQ(report.at("attempts")[0].at("entry_reasons"), day.at("entry_reasons"));
  // A day whose window never opens still says so.
  auto closed = catalogue();
  closed["definitions"]["batch"]["versions"][0]["window"]["weekdays"] = {2, 3, 4, 5};
  auto monday = server::parse_backtest({{"playbook", "batch@1"}, {"plan", "eod-50k"},
      {"days", {{{"file", (storage.directory / "2026-09-14.oprec").string()}}}}}, closed, {}, {}, false);
  monday.analytics.fallback_rate = 0;
  const auto skipped = server::run_backtest(monday, storage.directory / "closed", cancel);
  ASSERT_EQ(skipped.at("status"), "completed") << skipped.dump();
  EXPECT_EQ(skipped.at("days")[0].at("entry_reasons"), json({{"batch:SPX", "Outside entry window"}}));
}
TEST(Backtest, EntryReasonsRetainSpecificMissingConditions) {
  test::RecordingFile storage;
  auto request = request_for({recorded_day(storage.directory, {2026, 9, 14})});
  request.playbooks["definitions"]["batch"]["versions"][0]["conditions"] = {{"iv_rank", {{"min", .3}, {"max", 1}}}};
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "missing-iv", cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  const json expected{{"batch:SPX", "IV rank unavailable (no IV history)"}};
  EXPECT_EQ(report.at("days")[0].at("entry_reasons"), expected);
  EXPECT_EQ(report.at("attempts")[0].at("entry_reasons"), expected);
}
TEST(Backtest, GeneratedDaysMatchSingleReplayAndParallelReportsAreByteIdentical) {
  test::RecordingFile storage;
  auto definitions = catalogue();
  definitions["definitions"]["batch"]["versions"][0]["structure"]["template"]["target"] = {{"mode", "delta"}, {"value", 15}};
  auto request = server::parse_backtest({{"playbook", "batch"}, {"plan", "eod-50k"}, {"scenarios", 2}, {"seed", "18446744073709551610"}},
      definitions, providers::builtin_scenarios(), {});
  // SPX-only custom sources keep the scenario test small and remain verifiable.
  for (std::size_t index = 0; index < request.days.size(); ++index) {
    auto source = json::parse(request.days[index].scenario->source);
    source["symbols"] = {"SPX"};
    const auto file = storage.directory / ("scenario-" + std::to_string(index) + ".json");
    std::ofstream(file) << source.dump();
    request.days[index].scenario = providers::read_scenario(file);
  }
  const std::atomic_bool cancel{false};
  request.workers = 1;
  const auto sequential = server::run_backtest(request, storage.directory / "sequential", cancel);
  ASSERT_EQ(sequential.at("status"), "completed") << sequential.dump();
  ASSERT_FALSE(sequential.at("days")[0].at("fills").empty()) << sequential.dump();
  request.workers = 2;
  const auto parallel = server::run_backtest(request, storage.directory / "parallel", cancel);
  EXPECT_EQ(sequential.dump(), parallel.dump());
  EXPECT_EQ(bytes(storage.directory / "sequential/report.json"), bytes(storage.directory / "parallel/report.json"));
  for (std::size_t index = 0; index < request.days.size(); ++index) {
    const auto journal = sequential.at("days")[index].at("journal").get<std::string>();
    EXPECT_EQ(bytes(storage.directory / "sequential" / journal), bytes(storage.directory / "parallel" / journal));
    // verify_run replays one generated day through an independent Desk and
    // compares every transaction, all fills, final equity and the head hash.
    const auto verified = server::verify_run(storage.directory / "sequential" / journal);
    EXPECT_TRUE(verified.matched) << verified.message;
    EXPECT_EQ((verified.equity - request.config.initial_cash).str(), sequential.at("days")[index].at("pnl").get<std::string>());
  }
}
TEST(Backtest, CancellationPreservesCompletedDaysAndNeverInventsAttempts) {
  test::RecordingFile storage;
  auto request = request_for({recorded_day(storage.directory, {2026, 9, 14}), recorded_day(storage.directory, {2026, 9, 15})});
  request.workers = 1;
  std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "cancelled", cancel,
      [&](std::size_t completed, std::string_view) { if (completed == 1) cancel = true; });
  EXPECT_EQ(report.at("status"), "cancelled");
  EXPECT_EQ(report.at("summary").at("completed_days"), 1);
  EXPECT_TRUE(report.at("days")[1].is_null());
  EXPECT_TRUE(report.at("attempts").empty());
  const auto verified = server::verify_run(storage.directory / "cancelled/days/000001.jsonl");
  EXPECT_TRUE(verified.matched) << verified.message;
  const auto pre_cancelled = server::run_backtest(request, storage.directory / "pre-cancelled", cancel);
  EXPECT_EQ(pre_cancelled.at("summary").at("completed_days"), 0);
  cancel = false;
  const auto during_attempt = server::run_backtest(request, storage.directory / "cancelled-attempt", cancel,
      [&](std::size_t completed, std::string_view) { if (completed == 3) cancel = true; });
  EXPECT_EQ(during_attempt.at("status"), "cancelled");
  EXPECT_TRUE(during_attempt.at("attempts").empty());
  const auto prefix = server::verify_run(storage.directory / "cancelled-attempt/attempts/000001.jsonl");
  EXPECT_TRUE(prefix.matched) << prefix.message;
}
TEST(Backtest, ExistingSingleSourceJournalStillLoadsWithoutTransitionRecords) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  const auto journal = storage.directory / "legacy.jsonl";
  md::RecordingReader reader(file);
  server::Desk::Options options;
  options.replay = true;
  options.paper_journal = journal;
  options.run_input = server::recording_input(file);
  server::Desk desk("replay (simulated test)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  while (const auto batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  desk.stop();
  const auto recovery = trading::FileJournal::read(journal.string());
  EXPECT_EQ(trading::TradingSession::recover(recovery).snapshot()->equity, options.paper.initial_cash);
  EXPECT_EQ(bytes(journal).find("\"kind\":\"source\""), std::string::npos);
  const auto verified = server::verify_run(journal);
  EXPECT_TRUE(verified.matched) << verified.message;
}
TEST(Backtest, BrokenRecordingFailsWithoutCountingAZeroReturnDay) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  const auto request = request_for({file});
  std::ofstream(file, std::ios::trunc) << "broken";
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "broken", cancel);
  EXPECT_EQ(report.at("status"), "failed");
  EXPECT_EQ(report.at("summary").at("completed_days"), 0);
  EXPECT_EQ(report.at("errors").size(), 1U);
  EXPECT_TRUE(report.at("attempts").empty());
}
TEST(Backtest, RejectsInvalidInputsAndPinsArchivedHistoricalVersions) {
  auto definitions = catalogue();
  auto edited = definitions.at("definitions").at("batch").at("versions")[0];
  edited["version"] = 2; edited["name"] = "Latest";
  definitions["definitions"]["batch"]["versions"].push_back(edited);
  definitions["definitions"]["batch"]["deleted"] = true;
  const auto& scenarios = providers::builtin_scenarios();
  const json good{{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"scenarios", 2}, {"seed", "0"}};
  const auto parsed = server::parse_backtest(good, definitions, scenarios, {});
  EXPECT_EQ(parsed.playbooks.at("definitions").at("batch").at("versions").size(), 1U);
  EXPECT_EQ(parsed.days[1].seed, 1U);
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{{"plan", "practice"}, {"plan", "funded-eod-50k"},
      {"workers", 0}, {"workers", 17}, {"workers", 1.5}, {"scenarios", 253}, {"seed", "18446744073709551615"},
      {"seed", -1}, {"playbook", "batch@0"}, {"playbook", "batch@1junk"}, {"extra", true}}) {
    auto bad = good; bad[key] = value;
    EXPECT_THROW((void)server::parse_backtest(bad, definitions, scenarios, {}), std::exception) << key;
  }
  for (const auto& [key, message] : std::vector<std::pair<std::string, std::string>>{{"playbook", "playbook is required"}, {"plan", "plan is required"}}) {
    auto bad = good; bad.erase(key);
    try { (void)server::parse_backtest(bad, definitions, scenarios, {}); ADD_FAILURE() << key; }
    catch (const std::invalid_argument& error) { EXPECT_EQ(error.what(), message); }
  }
  auto unknown = good; unknown["playbook"] = "missing@1";
  try { (void)server::parse_backtest(unknown, definitions, scenarios, {}); ADD_FAILURE(); }
  catch (const std::invalid_argument& error) { EXPECT_STREQ(error.what(), "Unknown playbook"); }
  auto cashless = good; cashless["plan"] = {{"rules", json::object()}};
  try { (void)server::parse_backtest(cashless, definitions, scenarios, {}); ADD_FAILURE(); }
  catch (const std::invalid_argument& error) { EXPECT_STREQ(error.what(), "plan initial_cash is required"); }
  // A missing seed or day field, and a rule of the wrong type, are named too.
  for (const auto& [request, message] : std::vector<std::pair<json, std::string>>{
           {{{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"scenarios", 2}}, "seed is required"},
           {{{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"days", {{{"seed", "1"}}}}}, "day scenario is required"},
           {{{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"days", {{{"scenario", "reversal"}}}}}, "day seed is required"},
           {{{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"days", {{{"scenario", "hold-overnight"}, {"seed", "1"}}}}},
            "Scenario hold-overnight plays several sessions, and a backtest day is one; play it in Replay"},
           {{{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"days", {{{"scenario", "reversal"}, {"seed", "1"}, {"date", 20260916}}}}}, "date must be YYYY-MM-DD"},
           {{{"playbook", "batch@1"}, {"plan", {{"initial_cash", "1000"}, {"rules", {{"profit_target", 100}}}}}, {"scenarios", 1}, {"seed", 0}},
            "plan rules profit_target must be a decimal string"},
           {{{"playbook", "batch@1"}, {"plan", {{"initial_cash", "1000"}, {"rules", {{"profit_target", "100"}, {"buy_only", "yes"}}}}}, {"scenarios", 1}, {"seed", 0}},
            "plan rules buy_only must be true or false"},
           {{{"playbook", "batch@1"}, {"plan", {{"initial_cash", "1000"}, {"rules", {{"profit_target", "100"}, {"slippage_ticks", "1"}}}}}, {"scenarios", 1}, {"seed", 0}},
            "plan rules slippage_ticks must be an integer"},
           {{{"playbook", "batch@1"}, {"plan", {{"initial_cash", "1000"}, {"rules", {{"profit_target", "100"}, {"plan", 5}}}}}, {"scenarios", 1}, {"seed", 0}},
            "plan rules plan must be text"},
           {{{"playbook", "batch@1"}, {"plan", {{"initial_cash", "1000"}, {"rules", {{"profit_target", "100"}}}, {"fee_per_contract", 0.65}}}, {"scenarios", 1}, {"seed", 0}},
            "plan fee_per_contract must be a decimal string"}}) {
    try { (void)server::parse_backtest(request, definitions, scenarios, {}); ADD_FAILURE() << message; }
    catch (const std::invalid_argument& error) { EXPECT_EQ(error.what(), message); }
  }
  const auto custom = server::parse_backtest({{"playbook", "batch"}, {"plan", {{"initial_cash", "1000"},
      {"rules", {{"profit_target", "100"}, {"max_drawdown", "50"}, {"drawdown_mode", "end_of_day"}}}}}, {"scenarios", 1}, {"seed", 0}}, definitions, scenarios, {});
  EXPECT_EQ(custom.config.rules.max_drawdown, Money::parse("50"));
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  auto duplicate = good; duplicate.erase("scenarios"); duplicate.erase("seed");
  duplicate["days"] = {{{"file", file.string()}}, {{"file", file.string()}}};
  EXPECT_THROW((void)server::parse_backtest(duplicate, definitions, scenarios, {}, false), std::invalid_argument);
  duplicate["days"] = {{{"file", "../outside.oprec"}}};
  EXPECT_THROW((void)server::parse_backtest(duplicate, definitions, scenarios, storage.directory), std::invalid_argument);
  const auto confined = storage.directory / "recordings";
  std::filesystem::create_directory(confined);
  std::filesystem::create_symlink(file, confined / "escape.oprec");
  duplicate["days"] = {{{"file", "escape.oprec"}}};
  EXPECT_THROW((void)server::parse_backtest(duplicate, definitions, scenarios, confined), std::invalid_argument);
}
class BacktestSource final : public server::MetricsSource {
 public:
  using MetricsSource::trading_view;
  std::vector<std::string> symbols() const override { return {}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return {}; }
  server::EngineStatus status() const override { server::EngineStatus result; result.trading.write = "open"; return result; }
  std::shared_ptr<const server::TradingView> trading_view() const override {
    auto result = std::make_shared<server::TradingView>(); result->playbooks_json = catalogue().dump(); return result;
  }
};
server::ApiResponse call(server::BacktestHost& host, const server::ApiRequest& request, bool capture = true) {
  BacktestSource source;
  server::ApiResponse response;
  EXPECT_TRUE(host.handle(request, source, [&](server::ApiResponse value) { response = std::move(value); }));
  if (capture) test::capture_contract("backtests", request.method, request.target, response);
  return response;
}
TEST(Backtest, CustomFundedRulesCarryPayoutConsistencyAndBufferThroughTheRunner) {
  auto definitions = catalogue();
  const auto& scenarios = providers::builtin_scenarios();
  const json payouts{{"qualifying_profit", "50"}, {"qualifying_days", 1}, {"withdrawal_percent", 100},
      {"split_percent", 80}, {"minimum", "10"}, {"caps", {"500", "1000"}},
      {"consistency_percents", {20, 25, 30}}, {"buffer", "2100"}, {"buffer_payouts", 3}};
  json rules{{"phase", "funded"}, {"payouts", payouts}, {"max_drawdown", "3000"}};
  const auto parse = [&](const json& settings) {
    return server::parse_backtest({{"playbook", "batch"}, {"plan", {{"initial_cash", "50000"}, {"rules", settings}}},
                                  {"scenarios", 1}, {"seed", 0}}, definitions, scenarios, {});
  };
  auto parsed = parse(rules);
  EXPECT_EQ(parsed.config.rules.phase, trading::Phase::Funded);
  EXPECT_EQ(parsed.config.rules.payouts.consistency_percents, (std::vector<std::int64_t>{20, 25, 30}));
  EXPECT_EQ(parsed.config.rules.payouts.buffer, Money::parse("2100"));
  EXPECT_EQ(parsed.config.rules.payouts.buffer_payouts, 3);
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{
      {"buffer", "-1"}, {"buffer", 1}, {"buffer_payouts", 101}, {"buffer_payouts", 1.5},
      {"consistency_percents", {0}}, {"consistency_percents", {101}}, {"consistency_percents", {40.5}},
      {"consistency_percents", {"40"}}, {"consistency_percents", 40}, {"unknown", 1}}) {
    auto invalid = rules;
    invalid["payouts"][key] = value;
    EXPECT_THROW((void)parse(invalid), std::exception) << key << ": " << value;
  }
  auto invalid = rules;
  invalid["phase"] = "evaluation";
  EXPECT_THROW((void)parse(invalid), std::invalid_argument);
  invalid = rules; invalid.erase("payouts");
  EXPECT_THROW((void)parse(invalid), std::invalid_argument);
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  parsed.days = request_for({file}).days;
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(parsed, storage.directory / "funded", cancel);
  EXPECT_EQ(report.at("status"), "completed") << report.dump();
  ASSERT_FALSE(report.at("attempts").empty());
  EXPECT_EQ(report.at("attempts").at(0).at("outcome"), "open");
}

TEST(Backtest, CustomPlansAcceptAndValidateAccountMargin) {
  auto definitions = catalogue();
  const auto& scenarios = providers::builtin_scenarios();
  json rules{{"profit_target", "100"}, {"buying_power", true}, {"account_type", "ira"}, {"house_margin_percent", 25}, {"pm_vol_shock", 5}};
  const auto parse = [&](const json& settings) {
    return server::parse_backtest({{"playbook", "batch"}, {"plan", {{"initial_cash", "10000"}, {"rules", settings}}},
                                  {"scenarios", 1}, {"seed", 0}}, definitions, scenarios, {});
  };
  const auto parsed = parse(rules);
  EXPECT_EQ(parsed.config.rules.account_type, trading::AccountType::Ira);
  EXPECT_EQ(parsed.config.rules.house_margin_percent, 25);
  EXPECT_EQ(parsed.config.rules.pm_vol_shock, 5);
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{
      {"account_type", "unknown"}, {"house_margin_percent", 401}, {"pm_vol_shock", 51},
      {"pm_vol_shock", 1.5}, {"margin", "portfolio"}, {"buying_power", false}}) {
    auto invalid = rules;
    invalid[key] = value;
    EXPECT_THROW((void)parse(invalid), std::exception);
  }
}

TEST(BacktestApi, ContractFixture) {
  test::RecordingFile storage;
  const server::BacktestHost::Options options{storage.directory / "reports", storage.directory, {}, {}, {}, true};
  std::string id;
  {
    server::BacktestHost host(options);
    EXPECT_EQ(call(host, {"GET", "/api/backtests"}).status, 200);
    const json body{{"playbook", "batch"}, {"plan", "eod-50k"}, {"scenarios", 2}, {"seed", "1"}};
    const auto started = call(host, {"POST", "/api/backtests", body.dump()});
    ASSERT_EQ(started.status, 202) << started.body;
    id = json::parse(started.body).at("id");
    EXPECT_EQ(call(host, {"POST", "/api/backtests", body.dump()}).status, 409);
    EXPECT_EQ(call(host, {"DELETE", "/api/backtests/" + id + "?purge=true"}).status, 409);
    EXPECT_EQ(call(host, {"GET", "/api/backtests/compare?ids=" + id + ",000099"}).status, 409);
    EXPECT_EQ(call(host, {"PUT", "/api/backtests/" + id, "{\"keep\":true}"}).status, 200);
    EXPECT_EQ(call(host, {"DELETE", "/api/backtests/" + id}).status, 200);
    // Malformed routes and unsupported methods are not OpenAPI operations.
    EXPECT_EQ(call(host, {"GET", "/api/backtests/../../escape"}, false).status, 404);
  }
  server::BacktestHost reopened(options);
  const auto saved = call(reopened, {"GET", "/api/backtests/" + id});
  ASSERT_EQ(saved.status, 200) << saved.body;
  EXPECT_EQ(json::parse(saved.body).at("status"), "cancelled");
  EXPECT_EQ(call(reopened, {"DELETE", "/api/backtests/" + id}).status, 409);
  EXPECT_EQ(json::parse(saved.body).at("keep"), true);
  EXPECT_GT(json::parse(saved.body).at("bytes").get<std::uintmax_t>(), 0U);
  EXPECT_EQ(call(reopened, {"DELETE", "/api/backtests/" + id + "?purge=true"}).status, 200);
  EXPECT_FALSE(std::filesystem::exists(options.directory / id));
  EXPECT_EQ(call(reopened, {"GET", "/api/backtests/" + id}).status, 404);
  EXPECT_EQ(call(reopened, {"PUT", "/api/backtests"}, false).status, 405);
  server::BacktestHost disabled({storage.directory / "disabled", {}, {}, {}, {}, false});
  EXPECT_EQ(call(disabled, {"POST", "/api/backtests", "{}"}).status, 403);
}
TEST(BacktestApi, ListingSummarizesSavedReportIdentityWithoutCopyingTrades) {
  test::RecordingFile storage;
  const auto root = storage.directory / "000001";
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request_for({recorded_day(storage.directory, {2026, 9, 14})}), root, cancel);
  ASSERT_EQ(report.at("status"), "completed");
  server::write_backtest_report(root / "report.json", report);
  const json state{{"id", "000001"}, {"status", "completed"}, {"phase", "finished"}, {"completed", 2}, {"total", 2},
      {"label", server::kBacktestLabel}, {"report", nullptr}};
  server::write_backtest_report(root / "state.json", state);
  server::BacktestHost host({storage.directory, {}, {}, {}, {}, true});
  const auto listing = call(host, {"GET", "/api/backtests"});
  ASSERT_EQ(listing.status, 200) << listing.body;
  const auto run = json::parse(listing.body).at("runs")[0];
  EXPECT_EQ(run.at("playbook"), (json{{"id", "batch"}, {"version", 1}}));
  EXPECT_EQ(run.at("summary"), report.at("summary"));
  EXPECT_TRUE(run.at("report").is_null());
  EXPECT_EQ(json::parse(call(host, {"GET", "/api/backtests/000001"}).body).at("report"), report);
}
TEST(BacktestApi, ReplayScopeRequiredForStartAndCancelAndReadForProgress) {
  server::WritePolicy policy;
  policy.require_token = true;
  policy.tokens = {{"reader", {"read"}, "reader-secret"}, {"trader", {"read", "trade:*"}, "trader-secret"}, {"runner", {"read", "replay"}, "runner-secret"}};
  for (const auto& route : {"/api/backtests", "/api/backtests/000001"}) {
    for (const auto& method : {"POST", "PUT", "DELETE"}) {
      server::ApiRequest request{method, route, std::string(method) == "POST" ? "{}" : ""};
      request.content_type = "application/json";
      for (const auto& secret : {"reader-secret", "trader-secret"}) {
        request.authorization = std::string("Bearer ") + secret;
        const auto rejected = server::check_api_write(request, policy);
        ASSERT_TRUE(rejected); EXPECT_EQ(rejected->status, 403);
      }
      request.authorization = "Bearer runner-secret";
      EXPECT_FALSE(server::check_api_write(request, policy));
    }
    server::ApiRequest read{"GET", route}; read.authorization = "Bearer reader-secret";
    EXPECT_FALSE(server::check_api_write(read, policy));
  }
}

TEST(Backtest, DailyRowsIncludePlanLocks) {
  test::RecordingFile storage;
  auto request = request_for({recorded_day(storage.directory, {2026, 9, 14}), recorded_day(storage.directory, {2026, 9, 15})});
  request.config.rules.daily_loss_limit = Money::parse("10");
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "locked", cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  const auto& rows = report.at("attempts")[0].at("day_rows");
  ASSERT_EQ(rows.size(), 2U);
  for (const auto& row : rows) {
    EXPECT_EQ(row.at("day_lock"), "DAILY_LOSS_LIMIT");
    EXPECT_EQ(row.at("outcome"), "open");
    EXPECT_EQ(row.at("trades_opened"), 1);
    EXPECT_EQ(row.at("trades_closed"), 1);
    bool locked = false;
    for (const auto& trip : row.at("rule_trips")) if (trip.at("type") == "day_locked") locked = true;
    EXPECT_TRUE(locked);
  }
}
json comparison_report(const json& days) {
  return {{"schema", 1}, {"playbook", {{"id", "batch"}, {"version", 1}}}, {"config", {{"initial_cash", 50000000000LL}}},
      {"summary", json::object()}, {"days", days}};
}
json comparison_day(const std::string& date, const json& pnl) {
  return {{"date", date}, {"pnl", pnl}, {"input", {{"kind", "scenario"}, {"id", "fixture"}, {"seed", "1"}}}};
}
TEST(Backtest, ComparisonAlignsDatesAndSumsMicroDollarsWithoutInventingMarks) {
  auto first = comparison_report({comparison_day("2026-09-14", "0.000001"), comparison_day("2026-09-15", "-1.000003")});
  auto second = comparison_report({comparison_day("2026-09-14", "0.000002"), comparison_day("2026-09-16", "2.000001")});
  json runs = {{{"id", "000001"}, {"status", "completed"}, {"report", first}}, {{"id", "000002"}, {"status", "cancelled"}, {"report", second}}};
  auto result = server::compare_backtests(runs);
  EXPECT_EQ(result.at("different_inputs"), true);
  EXPECT_EQ(result.at("different_plans"), false);
  EXPECT_EQ(result.at("daily")[1].at("pnl").at("000002"), nullptr);
  EXPECT_EQ(result.at("combined").at("curve")[0].at("pnl"), "0.000003");
  EXPECT_EQ(result.at("combined").at("curve")[2].at("cumulative"), "1.000001");
  EXPECT_EQ(result.at("combined").at("max_drawdown"), "1.000003");
  EXPECT_EQ(result.at("combined").at("worst_days")[0].at("date"), "2026-09-15");
  EXPECT_DOUBLE_EQ(result.at("combined").at("day_win_rate"), 2.0 / 3.0);
  EXPECT_EQ(result.at("combined").at("daily_pnl").at("mean"), "0.333334");
  runs[1]["report"] = first;
  EXPECT_EQ(server::compare_backtests(runs).at("apples_to_oranges"), false);
  runs[1]["report"]["config"]["initial_cash"] = 1000000;
  runs[1]["report"]["days"][0]["pnl"] = nullptr;
  result = server::compare_backtests(runs);
  EXPECT_EQ(result.at("different_plans"), true);
  EXPECT_EQ(result.at("combined").at("curve")[0].at("pnl"), nullptr);
  EXPECT_EQ(result.at("combined").at("curve")[1].at("cumulative"), nullptr);
  EXPECT_EQ(result.at("combined").at("max_drawdown"), nullptr);
}
void saved_fixture(const std::filesystem::path& directory, const std::string& id, bool keep = false, const std::string& status = "completed") {
  std::filesystem::create_directories(directory / id);
  server::write_backtest_report(directory / id / "state.json", {{"id", id}, {"status", status}, {"keep", keep},
      {"phase", "finished"}, {"completed", 0}, {"total", 0}, {"label", server::kBacktestLabel}, {"report", nullptr}});
}
TEST(BacktestApi, RetentionRemovesOldestUnpinnedAndKeepsInterruptedPinsAcrossRestart) {
  test::RecordingFile storage;
  const auto directory = storage.directory / "reports";
  saved_fixture(directory, "000001", true, "running");
  saved_fixture(directory, "000002"); saved_fixture(directory, "000003"); saved_fixture(directory, "000004");
  server::BacktestHost::Options options{directory, storage.directory, {}, {}, {}, true, 1};
  {
    server::BacktestHost host(options);
    const auto listing = json::parse(call(host, {"GET", "/api/backtests"}).body);
    EXPECT_EQ(listing.at("runs")[3].at("status"), "interrupted");
    const auto started = call(host, {"POST", "/api/backtests", json{{"playbook", "batch"}, {"plan", "eod-50k"},
        {"days", {{{"file", recorded_day(storage.directory, {2026, 9, 14}).filename().string()}}}}}.dump()});
    ASSERT_EQ(started.status, 202) << started.body;
    EXPECT_EQ(json::parse(started.body).at("id"), "000005");
    EXPECT_TRUE(std::filesystem::exists(directory / "000001"));
    EXPECT_FALSE(std::filesystem::exists(directory / "000002"));
    EXPECT_FALSE(std::filesystem::exists(directory / "000003"));
    EXPECT_TRUE(std::filesystem::exists(directory / "000004"));
  }
  server::BacktestHost reopened(options);
  EXPECT_EQ(json::parse(call(reopened, {"GET", "/api/backtests/000001"}).body).at("keep"), true);
  EXPECT_EQ(call(reopened, {"PUT", "/api/backtests/000004", "{\"keep\":true}"}).status, 200);
  EXPECT_EQ(call(reopened, {"PUT", "/api/backtests/000004", "{\"keep\":1}"}).status, 400);
  EXPECT_EQ(call(reopened, {"DELETE", "/api/backtests/000005?purge=true"}).status, 200);
  options.keep = 0;
  server::BacktestHost unlimited(options);
  const auto started = call(unlimited, {"POST", "/api/backtests", json{{"playbook", "batch"}, {"plan", "eod-50k"},
      {"days", {{{"file", "2026-09-14.oprec"}}}}}.dump()});
  EXPECT_EQ(json::parse(started.body).at("id"), "000006");
  EXPECT_TRUE(std::filesystem::exists(directory / "000004"));
}
TEST(BacktestApi, PurgeAndKeepRefuseSymlinkEscapesAndUnknownRuns) {
  test::RecordingFile storage;
  const auto directory = storage.directory / "reports";
  saved_fixture(directory, "000001");
  saved_fixture(storage.directory / "outside", "000002");
  std::filesystem::create_directory_symlink(storage.directory / "outside/000002", directory / "000002");
  server::BacktestHost host({directory, {}, {}, {}, {}, true});
  EXPECT_EQ(call(host, {"DELETE", "/api/backtests/000002?purge=true"}).status, 409);
  EXPECT_EQ(call(host, {"PUT", "/api/backtests/000002", "{\"keep\":true}"}).status, 409);
  std::filesystem::create_directory_symlink(storage.directory / "outside", directory / "000001/escape");
  EXPECT_EQ(call(host, {"DELETE", "/api/backtests/000001?purge=true"}).status, 409);
  EXPECT_TRUE(std::filesystem::exists(storage.directory / "outside/000002/state.json"));
  EXPECT_EQ(call(host, {"DELETE", "/api/backtests/000009?purge=true"}).status, 404);
  EXPECT_EQ(call(host, {"DELETE", "/api/backtests/../outside?purge=true"}, false).status, 404);
  EXPECT_EQ(call(host, {"DELETE", "/api/backtests?purge=true"}).status, 400);
}
TEST(BacktestApi, ComparisonAcceptsLegacyReportsAndRejectsInvalidSelection) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  const std::atomic_bool cancel{false};
  auto report = server::run_backtest(request_for({file}), storage.directory / "source", cancel);
  report["schema"] = 1; report.erase("input_set");
  for (auto& attempt : report["attempts"]) attempt.erase("day_rows");
  for (const auto* id : {"000001", "000002"}) {
    saved_fixture(storage.directory, id);
    server::write_backtest_report(storage.directory / id / "report.json", report);
  }
  server::BacktestHost host({storage.directory, {}, {}, {}, {}, true});
  const auto saved = json::parse(call(host, {"GET", "/api/backtests/000001"}).body);
  EXPECT_EQ(saved.at("report"), report);
  std::uintmax_t size = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(storage.directory / "000001"))
    if (entry.is_regular_file()) size += entry.file_size();
  EXPECT_EQ(saved.at("bytes"), size);
  const auto compared = call(host, {"GET", "/api/backtests/compare?ids=000001,000002"});
  ASSERT_EQ(compared.status, 200) << compared.body;
  EXPECT_EQ(json::parse(compared.body).at("apples_to_oranges"), false);
  for (const auto* query : {"", "?ids=", "?ids=000001", "?ids=000001,000001", "?ids=000001,000002,", "?ids=../../escape,000001",
      "?ids=000001,000002,000003,000004,000005,000006,000007,000008,000009"}) {
    EXPECT_EQ(call(host, {"GET", std::string("/api/backtests/compare") + query}).status, 400);
  }
  EXPECT_EQ(call(host, {"GET", "/api/backtests/compare?ids=000001,000099"}).status, 404);
  saved_fixture(storage.directory, "000003");
  EXPECT_EQ(call(host, {"GET", "/api/backtests/compare?ids=000001,000003"}).status, 422);
}
}  // namespace

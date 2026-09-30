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
std::filesystem::path recorded_day(const std::filesystem::path& directory, md::Date date, bool loss = false) {
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
  for (int minute = 0; minute < 3; ++minute) {
    time = header.started + minute * md::kNanosPerMinute;
    const double high = minute == 0 ? 10 : loss && minute == 2 ? 13 : 9;
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
    EXPECT_EQ(call(host, {"DELETE", "/api/backtests/" + id}).status, 200);
    // Malformed routes and unsupported methods are not OpenAPI operations.
    EXPECT_EQ(call(host, {"GET", "/api/backtests/../../escape"}, false).status, 404);
  }
  server::BacktestHost reopened(options);
  const auto saved = call(reopened, {"GET", "/api/backtests/" + id});
  ASSERT_EQ(saved.status, 200) << saved.body;
  EXPECT_EQ(json::parse(saved.body).at("status"), "cancelled");
  EXPECT_EQ(call(reopened, {"DELETE", "/api/backtests/" + id}).status, 409);
  EXPECT_EQ(call(reopened, {"PUT", "/api/backtests"}, false).status, 405);
  server::BacktestHost disabled({storage.directory / "disabled", {}, {}, {}, {}, false});
  EXPECT_EQ(call(disabled, {"POST", "/api/backtests", "{}"}).status, 403);
}
TEST(BacktestApi, ReplayScopeRequiredForStartAndCancelAndReadForProgress) {
  server::WritePolicy policy;
  policy.require_token = true;
  policy.tokens = {{"reader", {"read"}, "reader-secret"}, {"trader", {"read", "trade:*"}, "trader-secret"}, {"runner", {"read", "replay"}, "runner-secret"}};
  for (const auto& route : {"/api/backtests", "/api/backtests/000001"}) {
    for (const auto& method : {"POST", "DELETE"}) {
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
}  // namespace

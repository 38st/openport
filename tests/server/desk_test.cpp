#include "support/contract_capture.hpp"
#include <fstream>
#include <future>
#include <sstream>
#include <gtest/gtest.h>

#include "openport/providers/replay.hpp"
#include "openport/server/desk.hpp"
#include "openport/server/engine.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/server/run.hpp"
#include "openport/server/plans.hpp"
#include "support/recording.hpp"
#include "support/scripted_market.hpp"
#include "server/run_json.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;
using namespace std::chrono_literals;

std::string read_file(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}
std::vector<md::Event> market_batch(const test::ScriptedMarket& market, double ask = 4.2) {
  return {md::ContractDefinition{0, market.contract},
          md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000},
          md::OptionQuote{0, market.time, ask - .2, ask, 20, 20},
          md::SnapshotComplete{"SPX", market.time}};
}
server::TradingReply command(server::Desk& desk, server::TradingCommand request, md::Timestamp time, md::Timestamp driver) {
  std::optional<server::TradingReply> result;
  desk.command(std::move(request), [&](server::TradingReply reply) { result = std::move(reply); }, time, driver);
  if (!result) throw std::runtime_error("Desk did not complete command");
  return *result;
}
TEST(Desk, ExplicitClockControlsFeedStallWithoutThreads) {
  test::ScriptedMarket market;
  server::Desk::Options options;
  options.analytics.fallback_rate = 0;
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  desk.replay_batch(market_batch(market), market.time);
  server::TradingCommand order;
  order.order = market.market("stale");
  auto result = command(desk, order, market.time, market.time + 10 * md::kNanosPerMinute);
  EXPECT_EQ(result.decision.code, trading::Reason::FEED_STALLED);
  order.order.client_order_id = "fresh";
  result = command(desk, order, market.time, market.time);
  EXPECT_TRUE(result.decision.ok()) << result.decision.message;
  ASSERT_EQ(result.view->snapshot->recent_fills.size(), 1U);
  EXPECT_EQ(result.view->snapshot->recent_fills.front().price, Money::parse("4.20"));
}
TEST(Desk, SnapshotsDoNotAnalyseOtherUnderlyingsBeforeTheirCompleteBoundary) {
  test::ScriptedMarket market;
  auto spy = *md::parse_osi("SPY261022C00500000");
  server::Desk::Options options;
  options.replay = true;
  server::Desk desk("test", {}, {{"SPX", "SPY"}}, options);
  desk.start_trading();
  auto batch = market_batch(market);
  batch.insert(batch.begin(), md::ContractDefinition{1, spy});
  batch.insert(batch.begin(), md::UnderlyingQuote{"SPY", market.time, 500, 500, 500});
  desk.replay_batch(batch, market.time);
  EXPECT_FALSE(desk.metrics("SPY"));
  desk.replay_batch({md::OptionQuote{1, market.time, 4, 4.2, 20, 20}, md::SnapshotComplete{"SPY", market.time}}, market.time);
  ASSERT_TRUE(desk.metrics("SPY"));
  EXPECT_EQ(desk.metrics("SPY")->options_priced, 1);
}

TEST(Desk, AnalyticsInputsAndEquityHooksUseExplicitMarketTime) {
  test::ScriptedMarket market;
  std::vector<server::EquitySample> samples;
  server::Desk::Options options;
  options.analytics.fallback_rate = 0;
  options.equity_sample = [&](std::string_view account, const server::EquitySample& sample) {
    EXPECT_EQ(account, "main");
    samples.push_back(sample);
  };
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  desk.replay_batch(market_batch(market), market.time);
  const auto analytics = desk.metrics("SPX");
  ASSERT_TRUE(analytics);
  EXPECT_THROW(desk.apply_analytics(analytics, market.time + 1), std::invalid_argument);
  desk.apply_analytics(analytics, market.time);
  server::TradingCommand order;
  order.order = market.market("sample");
  const auto reply = command(desk, order, market.time, market.time);
  ASSERT_TRUE(reply.decision.ok());
  ASSERT_FALSE(samples.empty());
  EXPECT_EQ(samples.back().time, market.time);
  EXPECT_EQ(samples.back().equity, reply.view->snapshot->equity);
}

void write_stream(const std::filesystem::path& file, bool snapshots) {
  test::ScriptedMarket market;
  auto header = test::recording_header();
  header.started = market.time;
  header.capabilities.delay = 0s;
  header.capabilities.poll_interval = snapshots ? 1s : 0s;
  md::Timestamp receipt = market.time;
  md::RecordingSink::Options options;
  options.clock = [&] { return receipt; };
  test::DiscardEvents discard;
  md::RecordingSink sink(file, header, discard, options);
  sink.publish(md::ContractDefinition{0, market.contract});
  for (int second = 0; second <= 12; ++second) {
    receipt = market.time + second * md::kNanosPerSecond;
    sink.publish(md::UnderlyingQuote{"SPX", receipt, 5000, 5000, 5000});
    sink.publish(md::OptionQuote{0, receipt, 100 + .1 * second, 100.2 + .1 * second, 20, 20});
    if (snapshots) sink.publish(md::SnapshotComplete{"SPX", receipt});
  }
  sink.close();
}
TEST(ReplayBatches, SnapshotAndStreamingBoundariesPreserveEveryQuote) {
  for (const bool snapshots : {false, true}) {
    test::RecordingFile file;
    write_stream(file.path, snapshots);
    md::RecordingReader reader(file.path);
    providers::ReplayBatches batches(reader, reader.header().subscription);
    std::size_t quotes = 0, count = 0;
    md::Timestamp previous = 0;
    while (const auto batch = batches.next()) {
      EXPECT_GT(batch->time, previous);
      previous = batch->time;
      for (const auto& event : batch->events) if (std::holds_alternative<md::OptionQuote>(event)) ++quotes;
      if (snapshots) { EXPECT_TRUE(std::holds_alternative<md::SnapshotComplete>(batch->events.back())); }
      ++count;
    }
    EXPECT_EQ(quotes, 13U);
    EXPECT_EQ(count, 13U);
  }
}

TEST(ReplayBatches, SubsecondStreamQuotesSettleOnTheFixedMarketSecond) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  auto header = test::recording_header();
  header.started = market.time;
  header.capabilities.poll_interval = 0s;
  header.capabilities.delay = 0s;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return market.time; };
  test::DiscardEvents discard;
  {
    md::RecordingSink sink(file.path, header, discard, recording);
    sink.publish(md::ContractDefinition{0, market.contract});
    sink.publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    market.time += md::kNanosPerSecond / 2;
    sink.publish(md::OptionQuote{0, market.time, 100, 100.2, 20, 20});
    sink.close();
  }
  server::Desk::Options options;
  options.replay = true;
  server::Desk desk("test", header.capabilities, header.subscription, options);
  desk.start_trading();
  md::RecordingReader reader(file.path);
  providers::ReplayBatches batches(reader, header.subscription);
  while (const auto batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  ASSERT_TRUE(desk.metrics("SPX"));
  EXPECT_EQ(desk.metrics("SPX")->as_of, market.time + md::kNanosPerSecond / 2);
  EXPECT_EQ(desk.market_time(), desk.metrics("SPX")->as_of);
  providers::ReplayProvider replay({file.path, 0, false, {}, 0, true});
  replay.set_driver([](providers::ReplayBatch) {
    std::promise<void> done;
    done.set_value();
    return done.get_future();
  });
  replay.start({header.subscription.underlyings, 0, 0}, discard);
  ASSERT_TRUE(test::recording_eventually([&] { return !replay.fast_forwarding(); }));
  EXPECT_THROW(replay.until(market.time), std::invalid_argument);
  EXPECT_NO_THROW(replay.until(market.time + md::kNanosPerSecond / 2));
  EXPECT_EQ(replay.settled_through(), market.time + md::kNanosPerSecond / 2);
  replay.stop();
}

class ImmediateClock final : public providers::ReplayClock {
 public:
  TimePoint now() override { return now_; }
  bool wait_until(TimePoint deadline, const std::atomic<bool>& stop) override { now_ = deadline; return !stop; }
  void interrupt() override {}
 private:
  TimePoint now_{};
};

void scripted_orders(server::Desk& desk, md::Timestamp open, md::Timestamp receipt) {
  const auto time = desk.market_time();
  server::TradingCommand request;
  if (time == open + md::kNanosPerMinute) {
    request.order.client_order_id = "vertical";
    request.order.quantity = 1;
    request.order.type = trading::OrderType::Market;
    request.order.tif = trading::TimeInForce::Ioc;
    request.order.legs = {{md::parse_osi("SPXW261127C06000000")->osi_symbol(), trading::Side::Buy, 1}, {md::parse_osi("SPXW261127C06010000")->osi_symbol(), trading::Side::Sell, 1}};
  } else if (time == open + 2 * md::kNanosPerMinute) {
    request.order.client_order_id = "bracket";
    request.order.symbol = md::parse_osi("SPXW261127P06000000")->osi_symbol();
    request.order.quantity = 1;
    request.order.type = trading::OrderType::Market;
    request.order.tif = trading::TimeInForce::Ioc;
    request.order.bracket = trading::Bracket{
        trading::ExitSpec{trading::Trigger{trading::TriggerSource::Underlying, trading::TriggerDirection::AtOrBelow, Money::parse("5800")}, {}},
        trading::ExitSpec{{}, Money::parse("200")}};
  } else if (time == open + 10 * md::kNanosPerMinute) request.kind = server::TradingCommand::Kind::ClosePositions;
  else return;
  const auto reply = command(desk, request, time, receipt);
  EXPECT_TRUE(reply.error_code.empty()) << reply.error_code;
  EXPECT_TRUE(reply.decision.ok()) << reply.decision.message;
}

TEST(ReproducibleRun, ScenarioDayHasIdenticalBytesAcrossSpeedsFastForwardAndVerification) {
  test::RecordingFile file;
  const auto source = file.directory / "golden.json";
  {
    std::ofstream out(source);
    out << R"({"id":"golden","title":"Golden day","description":"Simulated test day","symbols":["SPX"],"session":"regular","date":"2026-11-27","seed":81723,"generator":1,"drift":[[1,0.001]],"volatility":0.12,"iv_shift":0,"spot_vol":-2})";
  }
  const auto scenario = providers::read_scenario(source);
  providers::write_scenario_recording(file.path, scenario, scenario.date, scenario.seed);
  const auto open = providers::scenario_open(scenario, scenario.date);
  std::string golden;
  std::vector<trading::Fill> fills;
  Money equity;
  for (const int speed : {-1, 1, 300, 0}) {
    const auto journal = file.directory / ("run-" + std::to_string(speed) + ".jsonl");
    md::RecordingReader reader(file.path);
    server::Desk::Options options;
    options.replay = true;
    options.run_input = server::scenario_input(scenario, scenario.date, scenario.seed);
    options.paper_journal = journal;
    server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    if (speed == -1) {
      providers::ReplayBatches batches(reader, reader.header().subscription);
      while (const auto batch = batches.next()) {
        desk.replay_batch(batch->events, batch->received, batch->time);
        scripted_orders(desk, open, batch->received);
      }
    } else {
      providers::ReplayProvider replay({file.path, speed, false, std::make_shared<ImmediateClock>()});
      replay.set_driver([&](providers::ReplayBatch batch) {
        std::promise<void> done;
        desk.replay_batch(batch.events, batch.received, batch.time);
        scripted_orders(desk, open, batch.received);
        done.set_value();
        return done.get_future();
      });
      test::DiscardEvents discard;
      replay.start(reader.header().subscription, discard);
      const auto deadline = std::chrono::steady_clock::now() + 60s;
      while (!replay.finished() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
      ASSERT_TRUE(replay.finished());
      replay.stop();
    }
    ASSERT_TRUE(desk.trading_status().enabled) << desk.trading_status().reason;
    const auto view = desk.trading_view();
    ASSERT_TRUE(view);
    EXPECT_TRUE(view->snapshot->positions.empty());
    EXPECT_TRUE(view->snapshot->open_orders.empty());
    ASSERT_GE(view->snapshot->recent_fills.size(), 6U);
    if (golden.empty()) {
      golden = read_file(journal);
      fills = view->snapshot->recent_fills;
      equity = view->snapshot->equity;
      json summary = json::array();
      for (const auto& fill : fills) summary.push_back({fill.symbol, fill.quantity, fill.price.micros(), fill.fee.micros(), fill.time});
      // Pin money, quantities and fill times independently of analytic floats.
      EXPECT_EQ(summary, json::parse(R"([
        ["SPXW  261127C06000000",1,11700000,650000,1795789860000000000],
        ["SPXW  261127C06010000",1,5200000,650000,1795789860000000000],
        ["SPXW  261127P06000000",1,4100000,650000,1795789920000000000],
        ["SPXW  261127C06010000",1,4800000,650000,1795790400000000000],
        ["SPXW  261127C06000000",1,10500000,650000,1795790400000000000],
        ["SPXW  261127P06000000",1,4200000,650000,1795790400000000000]
      ])"));
      EXPECT_EQ(equity.micros(), 99926100000LL);
    } else {
      EXPECT_TRUE(read_file(journal) == golden);
      EXPECT_EQ(json(view->snapshot->recent_fills), json(fills));
      EXPECT_EQ(view->snapshot->equity, equity);
    }
    desk.stop();
  }
  const auto journal = file.directory / "run-0.jsonl";
  const auto calendar = md::scheduled_days();
  md::set_scheduled_days({{scenario.date, "Different host calendar", true, 13, 0}});
  const auto verified = server::verify_run(journal);
  EXPECT_EQ(md::scheduled_days().front().name, "Different host calendar");
  md::set_scheduled_days(calendar);
  EXPECT_TRUE(verified.matched) << verified.message;
  EXPECT_EQ(verified.equity, equity);
  const auto altered = file.directory / "altered.jsonl";
  std::uint64_t changed_at = 0;
  {
    auto output = trading::FileJournal::create(altered.string());
    for (const auto& record : trading::FileJournal::read(journal.string()).records) {
      auto payload = json::parse(record.payload);
      if (record.type == "market" && changed_at == 0) { payload["unexpected"] = true; changed_at = record.seq; }
      output->append(record.time, record.type, payload.dump());
    }
  }
  ASSERT_NE(changed_at, 0U);
  const auto mismatch = server::verify_run(altered);
  EXPECT_FALSE(mismatch.matched);
  EXPECT_NE(mismatch.message.find("First differing transaction " + std::to_string(changed_at)), std::string::npos) << mismatch.message;
  // The CLI takes the same path without starting a feed, HTTP or user directories.
  const auto cli = std::string(OPENPORT_APPS_DIR) + "/openportd --verify-run " + journal.string() + " > " + (file.directory / "verify.txt").string() + " 2>&1";
  EXPECT_EQ(std::system(cli.c_str()), 0);
  auto tampered = golden;
  const auto hash = tampered.find("\"hash\":\"");
  ASSERT_NE(hash, std::string::npos);
  tampered[hash + 8] = tampered[hash + 8] == 'a' ? 'b' : 'a';
  { std::ofstream out(journal); out << tampered; }
  EXPECT_FALSE(server::verify_run(journal).matched);
  EXPECT_NE(std::system(cli.c_str()), 0);
  { std::ofstream out(journal); out << golden; }
  { std::ofstream out(source, std::ios::app); out << '\n'; }
  const auto changed = server::verify_run(journal);
  EXPECT_FALSE(changed.matched);
  EXPECT_NE(changed.message.find("Scenario input changed"), std::string::npos) << changed.message;
}

server::ApiResponse replay_call(server::ReplayHost& host, std::string method, std::string target, json body = {}) {
  std::promise<server::ApiResponse> done;
  auto result = done.get_future();
  server::ApiRequest request{std::move(method), std::move(target), body.dump()};
  request.content_type = "application/json";
  request.actor = "test-actor";
  if (!host.handle(request, [&](server::ApiResponse response) { done.set_value(std::move(response)); }))
    throw std::runtime_error("Replay request was not handled");
  if (result.wait_for(5s) != std::future_status::ready) throw std::runtime_error("Replay request timed out");
  return result.get();
}
TEST(ReproducibleRun, LockstepSmallAndLargeStepsMatchContinuousCommandsAndVerify) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const test::ScriptedMarket market;
  std::string golden;
  // Continuous headless run is the reference, including identical command instants.
  for (const int step : {0, 1, 4}) {
    const auto directory = file.directory / std::to_string(step);
    std::filesystem::create_directory(directory);
    std::filesystem::path journal;
    if (step == 0) {
      journal = directory / "run.jsonl";
      md::RecordingReader reader(file.path);
      server::Desk::Options options;
      options.replay = true;
      options.initial_actor = "test-actor";
      options.run_input = server::recording_input(file.path);
      options.paper_journal = journal;
      // ReplayHost's default plan is practice.
      options.paper.rules = server::find_plan("practice")->rules;
      options.paper.initial_cash = server::find_plan("practice")->initial_cash;
      server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
      desk.start_trading();
      providers::ReplayBatches batches(reader, reader.header().subscription);
      while (const auto batch = batches.next()) {
        desk.replay_batch(batch->events, batch->received, batch->time);
        if (desk.market_time() == market.time + 4 * md::kNanosPerSecond) {
          server::TradingCommand order;
          order.actor = "test-actor";
          order.order = market.market("lockstep");
          EXPECT_TRUE(command(desk, order, desk.market_time(), batch->received).decision.ok());
        }
      }
      desk.stop();
      golden = read_file(journal);
    } else {
      server::Engine::Options options;
      options.paper_journal = directory / "main.jsonl";
      server::ReplayHost host({file.directory, options, false});
      const auto response = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}, {"speed", 0}});
      ASSERT_EQ(response.status, 201) << response.body;
      const auto id = json::parse(response.body).at("replay").at("id").get<std::string>();
      ASSERT_TRUE(test::recording_eventually([&] {
        return !json::parse(replay_call(host, "GET", "/api/replay").body).at("replay").at("fast_forwarding").get<bool>();
      }));
      for (int second = step; second <= 12; second += step) {
        const auto time = market.time + second * md::kNanosPerSecond;
        const auto target = second % 2 == 0 ? md::format_timestamp(time) : "10:00:" + (second < 10 ? std::string("0") : std::string()) + std::to_string(second);
        const auto stepped = replay_call(host, "PUT", "/api/replay", {{"until", target}});
        ASSERT_EQ(stepped.status, 200) << stepped.body;
        EXPECT_EQ(json::parse(stepped.body).at("settled_through"), md::format_timestamp(time));
        if (second == 4) {
          const auto bought = replay_call(host, "POST", "/api/replay/orders", {{"client_order_id", "lockstep"}, {"symbol", market.symbol()},
              {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}});
          ASSERT_EQ(bought.status, 201) << bought.body;
          if (step == 1) {
            const auto gap = replay_call(host, "PUT", "/api/replay", {{"until", md::format_timestamp(time + md::kNanosPerSecond / 2)}});
            ASSERT_EQ(gap.status, 200) << gap.body;
            const auto preview = replay_call(host, "POST", "/api/replay/orders/preview", {{"client_order_id", "preview"},
                {"symbol", market.symbol()}, {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}});
            EXPECT_EQ(preview.status, 200) << preview.body;
          }
        }
      }
      host.stop();
      journal = directory / "replays" / (id + ".jsonl");
      EXPECT_TRUE(read_file(journal) == golden);
    }
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}
TEST(ReproducibleRun, LockstepWaitsForTheConsumerAndRejectsBackwardAndPastEndTimes) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const test::ScriptedMarket market;
  providers::ReplayProvider replay({file.path, 0, false, {}, 0, true});
  auto held = std::make_shared<std::promise<void>>();
  std::atomic<bool> received{false};
  replay.set_driver([&](providers::ReplayBatch batch) {
    if (batch.time == market.time + md::kNanosPerSecond) {
      received = true;
      return held->get_future();
    }
    std::promise<void> done;
    done.set_value();
    return done.get_future();
  });
  test::DiscardEvents discard;
  replay.start({{"SPX"}}, discard);
  ASSERT_TRUE(test::recording_eventually([&] { return !replay.fast_forwarding(); }));
  auto advancing = std::async(std::launch::async, [&] { replay.until(market.time + md::kNanosPerSecond); });
  ASSERT_TRUE(test::recording_eventually([&] { return received.load(); }));
  EXPECT_EQ(advancing.wait_for(20ms), std::future_status::timeout);
  EXPECT_EQ(replay.settled_through(), market.time);
  held->set_value();
  ASSERT_EQ(advancing.wait_for(5s), std::future_status::ready);
  EXPECT_NO_THROW(advancing.get());
  EXPECT_TRUE(replay.paused());
  EXPECT_EQ(replay.settled_through(), market.time + md::kNanosPerSecond);
  EXPECT_THROW(replay.until(market.time), std::invalid_argument);
  EXPECT_THROW(replay.until(market.time + 20 * md::kNanosPerSecond), std::invalid_argument);
  EXPECT_EQ(replay.settled_through(), market.time + 12 * md::kNanosPerSecond);
  replay.stop();
}

TEST(ReproducibleRun, MissingRecordingAndOldJournalsHaveClearDiagnostics) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const auto journal = file.directory / "run.jsonl";
  {
    md::RecordingReader reader(file.path);
    server::Desk::Options options;
    options.run_input = server::recording_input(file.path);
    options.replay = true;
    options.paper_journal = journal;
    server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    providers::ReplayBatches batches(reader, reader.header().subscription);
    desk.replay_batch(batches.next()->events, reader.header().started);
    desk.set_dividends({{"SPX", {2026, 9, 23}, Money::parse("1.00")}});
  }
  EXPECT_TRUE(server::verify_run(journal).matched);
  { std::ofstream out(file.path, std::ios::app); out << 'x'; }
  EXPECT_NE(server::verify_run(journal).message.find("Recording input changed"), std::string::npos);
  std::filesystem::remove(file.path);
  EXPECT_NE(server::verify_run(journal).message.find("Missing input"), std::string::npos);
  const auto old = file.directory / "old.jsonl";
  { trading::TradingSession session({}, 0, trading::FileJournal::create(old.string())); }
  EXPECT_NO_THROW((void)trading::TradingSession::recover(trading::FileJournal::read(old.string())));
  EXPECT_NE(server::verify_run(old).message.find("older journals still load"), std::string::npos);
}
}  // namespace

namespace {
TEST(ReplayRun, ContractFixture) {
  test::RecordingFile file;
  write_stream(file.path, true);
  server::Engine::Options options;
  options.paper_journal = file.directory / "paper.jsonl";
  options.write_mode = "open";
  server::ReplayHost host({file.directory, options, false});
  const auto capture = [&](std::string method, std::string path, json body = json::object()) {
    const auto response = replay_call(host, method, path, body);
    EXPECT_GE(response.status, 200);
    EXPECT_LT(response.status, 300) << response.body;
    test::capture_contract("replay", method, path, response);
    return json::parse(response.body);
  };
  capture("GET", "/api/replay");
  const auto started = capture("POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}});
  const auto id = started.at("replay").at("id").get<std::string>();
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
  capture("PUT", "/api/replay", {{"until", "10:00:01"}});
  capture("GET", "/api/replay/status");
  capture("GET", "/api/replay/account");
  capture("DELETE", "/api/replay");
  capture("GET", "/api/replay");
  capture("GET", "/api/replay/history/" + id);
  capture("GET", "/api/replay/history/" + id + "/trades");
}
}  // namespace

namespace {
TEST(ReplayRun, CommandActorDefaultsForOlderInputs) {
  server::TradingCommand command;
  command.actor = "agent";
  auto recorded = json(command);
  EXPECT_EQ(recorded.get<server::TradingCommand>().actor, "agent");
  recorded.erase("actor");
  EXPECT_EQ(recorded.get<server::TradingCommand>().actor, "unknown");
}

TEST(ReplayRun, JournalsWithoutActorsStillVerifyTheirOriginalHashes) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const auto journal = file.directory / "run.jsonl";
  const test::ScriptedMarket market;
  {
    md::RecordingReader reader(file.path);
    server::Desk::Options options;
    options.run_input = server::recording_input(file.path);
    options.replay = true;
    options.paper_journal = journal;
    server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    providers::ReplayBatches batches(reader, reader.header().subscription);
    while (const auto batch = batches.next()) {
      desk.replay_batch(batch->events, batch->received, batch->time);
      if (desk.market_time() == market.time + 4 * md::kNanosPerSecond) {
        server::TradingCommand order;
        order.actor = "agent";
        order.order = market.market("older-order");
        ASSERT_TRUE(command(desk, order, desk.market_time(), batch->received).decision.ok());
      }
    }
  }
  const auto older = file.directory / "older.jsonl";
  const std::function<void(json&)> strip = [&](json& value) {
    if (value.is_object()) value.erase("actor");
    if (value.is_structured()) {
      for (auto& child : value) strip(child);
    }
  };
  {
    auto output = trading::FileJournal::create(older.string());
    for (const auto& record : trading::FileJournal::read(journal.string()).records) {
      auto payload = json::parse(record.payload);
      strip(payload);
      output->append(record.time, record.type, payload.dump());
    }
  }
  const auto recovery = trading::FileJournal::read(older.string());
  const auto restored = trading::TradingSession::recover(recovery);
  ASSERT_FALSE(restored.snapshot()->recent_fills.empty());
  EXPECT_EQ(restored.snapshot()->recent_fills.front().actor, "unknown");
  const auto verified = server::verify_run(older);
  EXPECT_TRUE(verified.matched) << verified.message;
  EXPECT_EQ(verified.head, recovery.head);
}
}  // namespace

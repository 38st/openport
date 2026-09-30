#include "support/recording.hpp"
#include "support/scripted_market.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "openport/providers/replay.hpp"
#include "openport/server/engine.hpp"
#include "openport/server/equity.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/server/sandboxes.hpp"

namespace {
using namespace openport;
using namespace std::chrono_literals;

class SyntheticProvider final : public md::Provider {
 public:
  std::string_view name() const noexcept override { return "synthetic-engine"; }
  md::Capabilities capabilities() const noexcept override { return {}; }
  void start(const md::Subscription&, md::EventSink& out) override {
    sink = &out;
    for (int snapshot = 0; snapshot < 3; ++snapshot) {
      const auto ts = as_of + snapshot * md::kNanosPerSecond;
      const double spot = 5000 + snapshot;
      sink->publish(md::UnderlyingQuote{"SPX", ts, spot - .5, spot + .5, spot});
      md::InstrumentId id = 0;
      for (const auto date :
           {md::Date{2026, 10, 22}, md::Date{2026, 12, 22}, md::Date{2027, 9, 22}})
        for (double strike = 4500; strike <= 5500; strike += 25)
          for (const auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
            auto contract = *md::parse_osi("SPXW261022C05000000");
            contract.expiry = date;
            contract.strike = strike;
            contract.type = type;
            if (snapshot == 0) sink->publish(md::ContractDefinition{id, contract});
            const auto years = md::years_between(ts, contract.expiry_time());
            const auto forward = spot * std::exp((.045 - .013) * years);
            const auto log_moneyness = std::log(strike / forward);
            const auto vol = .18 - .25 * log_moneyness + .9 * log_moneyness * log_moneyness;
            const auto price =
                pricing::black_price(type, forward, strike, years, vol, std::exp(-.045 * years));
            sink->publish(md::OptionQuote{id, ts, price - .05, price + .05, 10, 11});
            sink->publish(md::OpenInterest{id, ts, 500});
            sink->publish(md::VendorGreeks{id, ts, vol});
            sink->publish(md::OptionTrade{id, ts, price, 1});
            ++id;
          }
    }
    sink->publish(md::ProviderStatus{as_of, md::FeedState::Live, "complete", "SPX"});
  }
  void stop() override {
    if (sink) {
      sink->publish(md::ProviderStatus{as_of, md::FeedState::Stopped, "synthetic stopped", ""});
      sink = nullptr;
    }
  }
  const md::Timestamp as_of = md::new_york_to_utc({2026, 9, 22}, 15, 0);
  md::EventSink* sink = nullptr;
};

void same_analytics(const analytics::UnderlyingMetrics& expected,
                    const analytics::UnderlyingMetrics& actual) {
  EXPECT_EQ(expected.symbol, actual.symbol);
  EXPECT_EQ(expected.as_of, actual.as_of);
  test::exact_double(expected.spot, actual.spot);
  EXPECT_EQ(expected.spot_source, actual.spot_source);
  EXPECT_EQ(expected.options_priced, actual.options_priced);
  ASSERT_EQ(expected.slices.size(), actual.slices.size());
  for (std::size_t i = 0; i < expected.slices.size(); ++i) {
    const auto& a = expected.slices[i];
    const auto& b = actual.slices[i];
    EXPECT_EQ(a.expiry, b.expiry);
    test::exact_double(a.years, b.years);
    test::exact_double(a.forward.forward, b.forward.forward);
    test::exact_double(a.forward.discount, b.forward.discount);
    test::exact_double(a.atm_iv, b.atm_iv);
    ASSERT_EQ(a.strikes.size(), b.strikes.size());
    for (std::size_t j = 0; j < a.strikes.size(); ++j) {
      const auto& x = a.strikes[j];
      const auto& y = b.strikes[j];
      test::exact_double(x.strike, y.strike);
      test::exact_double(x.iv, y.iv);
      for (auto side : {&analytics::StrikeMetrics::call, &analytics::StrikeMetrics::put}) {
        const auto& c = x.*side;
        const auto& d = y.*side;
        test::exact_double(c.iv, d.iv);
        test::exact_double(c.bid_iv, d.bid_iv);
        test::exact_double(c.ask_iv, d.ask_iv);
        test::exact_double(c.open_interest, d.open_interest);
      }
    }
  }
}

TEST(EngineRecording, SyntheticSessionReplaysIdenticalSpotForwardsAndEveryExpiryIv) {
  test::RecordingFile file;
  SyntheticProvider provider;
  server::Engine::Options options;
  options.record_file = file.path;
  options.analytics_interval = 1h;  // EOF and shutdown must still compute the final snapshot.
  std::atomic<md::Timestamp> receipt{10 * md::kNanosPerDay};
  options.clock = [&] { return receipt.fetch_add(1); };
  server::Engine original(provider, {{"SPX"}}, options);
  original.start();
  original.stop();
  ASSERT_TRUE(original.recording_error().empty());
  const auto expected = original.metrics("SPX");
  ASSERT_TRUE(expected);
  ASSERT_EQ(expected->slices.size(), 3u);
  ASSERT_EQ(expected->options_priced, 246);
  EXPECT_EQ(expected->as_of, provider.as_of + 2 * md::kNanosPerSecond);
  md::RecordingReader reader(file.path);
  EXPECT_EQ(reader.header().started, 10 * md::kNanosPerDay);
  md::Timestamp previous = reader.header().started;
  std::size_t count = 0;
  while (auto record = reader.next()) {
    EXPECT_GT(record->received, previous);
    previous = record->received;
    ++count;
  }
  EXPECT_EQ(count, original.recording_stats().events);
  EXPECT_GT(count, original.status().events);  // records survive downstream coalescing
  EXPECT_TRUE(reader.diagnostic().empty());
  providers::ReplayProvider replay({.file = file.path, .speed = 0, .loop = false, .clock = {}});
  options.record_file.clear();
  options.clock = [] { return md::now(); };  // deliberately a different wall clock
  server::Engine replayed(replay, {{"SPX"}}, options);
  replayed.start();
  ASSERT_TRUE(test::recording_eventually([&] {
    return replayed.status().feed_message.find("end of recording") != std::string::npos &&
           replayed.metrics("SPX");
  }));
  replayed.stop();
  same_analytics(*expected, *replayed.metrics("SPX"));
}

// U4: uptime is wall time since the engine started, not time on the market clock a
// replay or the demo keeps (which put it at 107,099 s right after launch).
TEST(Engine, UptimeCountsWallTimeWhateverClockTheMarketKeeps) {
  using nlohmann::json;
  SyntheticProvider provider;
  server::Engine::Options options;
  options.paper_enabled = false;
  options.clock = [] { return md::new_york_to_utc({2020, 1, 2}, 9, 30); };
  const auto launched = std::chrono::steady_clock::now();
  std::atomic<int> seconds{0};
  options.monotonic_clock = [&] { return launched + std::chrono::seconds(seconds.load()); };
  server::Engine engine(provider, {{"SPX"}}, options);
  EXPECT_EQ(json::parse(server::handle_api({"GET", "/api/status"}, engine).body)["engine"]["uptime_seconds"], 0);
  engine.start();
  seconds = 90;
  EXPECT_EQ(json::parse(server::handle_api({"GET", "/api/status"}, engine).body)["engine"]["uptime_seconds"], 90);
  engine.stop();
}

server::ApiResponse call(server::ReplayHost& host, std::string method, std::string target, std::string body = {},
                         std::chrono::seconds timeout = 5min) {
  auto promise = std::make_shared<std::promise<server::ApiResponse>>();
  auto future = promise->get_future();
  server::ApiRequest request{std::move(method), std::move(target), std::move(body)};
  request.content_type = "application/json";
  EXPECT_TRUE(host.handle(request, [promise](server::ApiResponse response) { promise->set_value(std::move(response)); }));
  if (future.wait_for(timeout) != std::future_status::ready) throw std::runtime_error("replay request did not complete");
  return future.get();
}

TEST(ReplayHost, ListsStartsTradesControlsAndStopsARecordedSession) {
  using nlohmann::json;
  test::RecordingFile file;
  const auto recording = file.directory / "synthetic.oprec";
  {
    SyntheticProvider provider;
    server::Engine::Options options;
    options.record_file = recording;
    options.paper_enabled = false;
    // Received a moment after each quote's market time, as a live feed would be.
    std::atomic<md::Timestamp> receipt{provider.as_of};
    options.clock = [&] { return receipt.fetch_add(1); };
    server::Engine original(provider, {{"SPX"}}, options);
    original.start();
    original.stop();
    ASSERT_TRUE(original.recording_error().empty());
  }
  server::Engine::Options base;
  base.analytics_interval = 1ms;
  server::ReplayHost host({file.directory, base});
  server::ApiRequest other{"GET", "/api/status"};
  EXPECT_FALSE(host.handle(other, [](server::ApiResponse) {}));
  EXPECT_FALSE(host.handle({"GET", "/api/replayed"}, [](server::ApiResponse) {}));

  auto listing = json::parse(call(host, "GET", "/api/replay").body);
  ASSERT_EQ(listing["recordings"].size(), 1);
  EXPECT_EQ(listing["recordings"][0]["file"], "synthetic.oprec");
  EXPECT_EQ(listing["recordings"][0]["provider"], "synthetic-engine");
  EXPECT_EQ(listing["recordings"][0]["imported"], false);
  EXPECT_EQ(listing["recordings"][0]["symbols"], json::array({"SPX"}));
  EXPECT_TRUE(listing["replay"].is_null());
  EXPECT_EQ(call(host, "GET", "/api/replay/status").status, 404);

  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file": "../synthetic.oprec"})").status, 404);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file": "synthetic.oprec", "speed": 3})").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file": "synthetic.oprec", "plan": "funded-eod-50k"})").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file": "synthetic.oprec", "extra": 1})").status, 400);
  const auto started = call(host, "POST", "/api/replay", R"({"file": "synthetic.oprec", "speed": 0})");
  ASSERT_EQ(started.status, 201) << started.body;
  EXPECT_EQ(json::parse(started.body)["replay"]["file"], "synthetic.oprec");
  EXPECT_EQ(json::parse(started.body)["replay"]["speed"], 0);

  // The mirrored API serves the replay's own analytics and account.
  ASSERT_TRUE(test::recording_eventually([&] {
    return json::parse(call(host, "GET", "/api/replay").body)["replay"]["finished"] == true &&
           call(host, "GET", "/api/replay/underlyings/SPX/summary").status == 200;
  }));
  const auto status = json::parse(call(host, "GET", "/api/replay/status").body);
  EXPECT_EQ(status["trading"]["enabled"], true);
  EXPECT_EQ(status["provider"]["name"], "replay (synthetic-engine)");
  const auto symbol = *md::parse_osi("SPXW261022C05000000");
  const json order{{"client_order_id", "replayed"}, {"symbol", symbol.osi_symbol()}, {"side", "buy"},
                   {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}};
  const auto bought = call(host, "POST", "/api/replay/orders", order.dump());
  ASSERT_EQ(bought.status, 403) << bought.body;
  EXPECT_EQ(json::parse(bought.body)["error"]["code"], "REPLAY_READ_ONLY");

  const auto controlled = json::parse(call(host, "PUT", "/api/replay", R"({"speed": 60, "paused": true})").body);
  EXPECT_EQ(controlled["replay"]["speed"], 60);
  EXPECT_EQ(controlled["replay"]["paused"], true);
  EXPECT_EQ(call(host, "PUT", "/api/replay", R"({"speed": 7})").status, 400);
  EXPECT_EQ(call(host, "GET", "/api/replay?x=1").status, 400);
  const auto tick = json::parse(host.tick());
  EXPECT_EQ(tick["type"], "replay_tick");
  EXPECT_EQ(tick["replay"]["file"], "synthetic.oprec");
  EXPECT_EQ(tick["trading"]["enabled"], true);

  EXPECT_TRUE(json::parse(call(host, "DELETE", "/api/replay").body)["replay"].is_null());
  EXPECT_EQ(call(host, "GET", "/api/replay/status").status, 404);
  EXPECT_TRUE(host.tick().empty());
  EXPECT_EQ(call(host, "PUT", "/api/replay", R"({"paused": false})").status, 404);
}

/// The generated demo days of this process's replay hosts, in the temporary directory.
std::vector<std::filesystem::path> demo_directories() {
  std::vector<std::filesystem::path> found;
  const auto prefix = "openport-demo-" + std::to_string(::getpid()) + "-";
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::temp_directory_path()))
    if (entry.path().filename().string().starts_with(prefix)) found.push_back(entry.path());
  return found;
}

TEST(ReplayHost, PreparesTheDemoWhenListedAndRemovesItsDaysWithTheHost) {
  using nlohmann::json;
  test::RecordingFile file;
  {
    server::ReplayHost host({file.directory, {}});
    EXPECT_TRUE(demo_directories().empty());  // nothing is generated until asked
    ASSERT_EQ(call(host, "GET", "/api/replay").status, 200);
    ASSERT_TRUE(test::recording_eventually([] {
      const auto directories = demo_directories();
      std::error_code ec;
      return directories.size() == 1 && std::filesystem::exists(directories[0] / "1.oprec", ec);
    }));
  }
  EXPECT_TRUE(demo_directories().empty());
}

TEST(ReplayHost, PlaysTheSimulatedDemoMarketWithItsOwnAccount) {
  using nlohmann::json;
  test::RecordingFile file;
  server::Engine::Options base;
  base.analytics_interval = 1ms;
  base.series = std::make_shared<server::SeriesStore>();
  server::ReplayHost host({file.directory, base});
  const auto listing = json::parse(call(host, "GET", "/api/replay").body);
  EXPECT_EQ(listing["demo"]["provider"], "demo");
  EXPECT_EQ(listing["demo"]["symbols"], json::array({"SPX", "SPY", "QQQ"}));
  EXPECT_EQ(listing["demo"]["id"], "reversal");
  ASSERT_EQ(listing["demos"].size(), 14);
  EXPECT_EQ(listing["demos"][4]["id"], "overnight");
  EXPECT_EQ(listing["demos"][4]["symbols"], json::array({"SPX"}));
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"demo": "sideways"})").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"demo": true, "file": "synthetic.oprec"})").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", "{}").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"demo": 1})").status, 400);

  // Generated on start; at 1x the opening snapshot plays at once and the next waits 15 seconds.
  const auto started = call(host, "POST", "/api/replay", R"({"demo": true, "seed": "scenario", "plan": "intraday-25k"})", 60s);
  ASSERT_EQ(started.status, 201) << started.body;
  const auto replay = json::parse(started.body)["replay"];
  EXPECT_EQ(replay["demo"], true);
  EXPECT_EQ(replay["file"], "Demo market: Slide and rebound");
  EXPECT_EQ(replay["provider"], "demo");

  // Next month's SPY series are the last defined: wait until the one traded here is quoted.
  ASSERT_TRUE(test::recording_eventually([&] {
    const auto chain = call(host, "GET", "/api/replay/underlyings/SPY/chain?expiry=2026-10-16PM");
    if (chain.status != 200) return false;
    const auto parsed = json::parse(chain.body);
    for (const auto& row : parsed["strikes"])
      if (row["strike"] == 600 && row["call"].is_object() && row["call"]["ask"].is_number()) return true;
    return false;
  }));
  const auto status = json::parse(call(host, "GET", "/api/replay/status").body);
  EXPECT_EQ(status["series"]["enabled"], false);
  EXPECT_TRUE(base.series->rows("SPX", 0, md::now()).empty());
  EXPECT_EQ(status["provider"]["name"], "replay (demo)");
  EXPECT_EQ(status["provider"]["simulated"], true);
  EXPECT_EQ(status["trading"]["plan"], "Intraday 25K");
  const json order{{"client_order_id", "demo"}, {"symbol", md::parse_osi("SPY261016C00600000")->osi_symbol()},
                   {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}};
  const auto bought = call(host, "POST", "/api/replay/orders", order.dump());
  ASSERT_EQ(bought.status, 201) << bought.body;
  EXPECT_EQ(json::parse(bought.body)["order"]["status"], "filled");
  EXPECT_EQ(json::parse(host.tick())["replay"]["demo"], true);
  host.stop();
  // Started again, the day is already generated.
  const auto again = std::chrono::steady_clock::now();
  ASSERT_EQ(call(host, "POST", "/api/replay", R"({"demo": true, "seed": "scenario"})", 60s).status, 201);
  EXPECT_LT(std::chrono::steady_clock::now() - again, 500ms);
  host.stop();

  server::ReplayHost off({file.directory, base, false});
  EXPECT_TRUE(json::parse(call(off, "GET", "/api/replay").body)["demo"].is_null());
  EXPECT_EQ(call(off, "POST", "/api/replay", R"({"demo": true})").status, 404);
}

TEST(EngineRecording, ExistingFileFailsBeforeProviderStarts) {
  test::RecordingFile file;
  test::record_events(file.path, {});
  SyntheticProvider provider;
  server::Engine::Options options;
  options.record_file = file.path;
  server::Engine engine(provider, {{"SPX"}}, options);
  EXPECT_THROW(engine.start(), std::runtime_error);
  EXPECT_EQ(provider.sink, nullptr);
}

TEST(EngineRecording, WriteFailureStaysVisibleAfterHealthyQuotesAndCleanShutdown) {
  test::RecordingFile file;
  SyntheticProvider provider;
  server::Engine::Options options;
  options.record_file = file.path;
  options.recording.frame_bytes = 1;
  options.recording.write = [writes = 0](int fd, std::span<const char> bytes) mutable {
    if (++writes > 1) throw std::runtime_error("injected ENOSPC");
    if (::write(fd, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size()))
      throw std::runtime_error("test write failed");
  };
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  ASSERT_TRUE(test::recording_eventually([&] { return !engine.recording_error().empty(); }));
  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Live, "healthy", "SPX"});
  engine.stop();
  const auto status = engine.status();
  EXPECT_EQ(status.feed_state, md::FeedState::Error);
  EXPECT_NE(status.feed_message.find("ENOSPC"), std::string::npos);
  EXPECT_EQ(status.underlyings.at("SPX").state, md::FeedState::Error);
  EXPECT_TRUE(engine.metrics("SPX"));
}

TEST(EngineRecording, ConsumerLaunchFailureClosesRecordingAfterStoppingProvider) {
  test::RecordingFile file;
  SyntheticProvider provider;
  server::Engine::Options options;
  options.record_file = file.path;
  options.launch = [](auto) -> std::thread { throw std::runtime_error("injected launch failure"); };
  server::Engine engine(provider, {{"SPX"}}, options);
  EXPECT_THROW(engine.start(), std::runtime_error);
  EXPECT_EQ(provider.sink, nullptr);
  md::RecordingReader reader(file.path);
  std::optional<md::RecordedEvent> last;
  while (auto record = reader.next()) last = std::move(record);
  ASSERT_TRUE(last);
  EXPECT_EQ(std::get<md::ProviderStatus>(last->event).state, md::FeedState::Stopped);
  EXPECT_TRUE(reader.diagnostic().empty());
}

/// A reproducible storage/publish benchmark, with no timing threshold in CI.
/// 30,000 contracts (60 expiries, 250 strikes, call/put), 30 quote updates each.
TEST(RecordingPerformance, SyntheticChain) {
  test::RecordingFile file;
  auto publish = [](md::EventSink& sink) {
    sink.publish(md::UnderlyingQuote{"SPX", 1, 5000, 5001, 5000.5});
    auto contract = *md::parse_osi("SPXW261022C05000000");
    for (md::InstrumentId id = 0; id < 30000; ++id) {
      contract.expiry = md::date_from_days(md::days_since_epoch({2026, 10, 22}) + id / 500);
      contract.strike = 4000 + static_cast<double>((id / 2) % 250) * 10;
      contract.type = id % 2 == 0 ? pricing::OptionType::Call : pricing::OptionType::Put;
      sink.publish(md::ContractDefinition{id, contract});
    }
    for (int update = 0; update < 30; ++update)
      for (md::InstrumentId id = 0; id < 30000; ++id) {
        const double price = 100 + static_cast<double>(id % 250) * .17 + update * .01;
        sink.publish(
            md::OptionQuote{id, 1'790'000'000 * md::kNanosPerSecond + update * md::kNanosPerSecond,
                            price, price + .1, 10, 11});
      }
  };
  constexpr double count = 930001;
  md::EventQueue baseline;
  auto started = std::chrono::steady_clock::now();
  publish(baseline);
  const double baseline_ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started).count() /
      count;
  md::EventQueue downstream;
  md::RecordingSink recorder(file.path, test::recording_header(), downstream, {});
  started = std::chrono::steady_clock::now();
  publish(recorder);
  const auto publish_finished = std::chrono::steady_clock::now();
  recorder.close();
  const auto finished = std::chrono::steady_clock::now();
  ASSERT_TRUE(recorder.error().empty()) << recorder.error();
  const auto stats = recorder.stats();
  EXPECT_EQ(stats.events, static_cast<std::uint64_t>(count));
  EXPECT_EQ(stats.bytes, std::filesystem::file_size(file.path));
  const double total_ns =
      std::chrono::duration<double, std::nano>(finished - started).count() / count;
  const double publish_ns =
      std::chrono::duration<double, std::nano>(publish_finished - started).count() / count;
  std::printf(
      "recording benchmark: events=%llu bytes=%llu bytes/event=%.3f sink_ns/event=%.1f "
      "publish_ns/event=%.1f drained_ns/event=%.1f baseline_ns/event=%.1f overhead_ns/event=%.1f\n",
      static_cast<unsigned long long>(stats.events), static_cast<unsigned long long>(stats.bytes),
      static_cast<double>(stats.bytes) / count,
      static_cast<double>(stats.publish_nanoseconds) / count, publish_ns, total_ns, baseline_ns,
      total_ns - baseline_ns);
  md::RecordingReader reader(file.path);
  std::uint64_t recovered = 0;
  while (reader.next()) ++recovered;
  EXPECT_EQ(recovered, stats.events);
  EXPECT_TRUE(reader.diagnostic().empty());
}

void drill_recording(const std::filesystem::path& path) {
  auto header = test::recording_header();
  const md::Date date{2026, 9, 16};
  header.started = md::new_york_to_utc(date, 9, 30);
  md::Timestamp now = header.started;
  md::RecordingSink::Options options;
  options.clock = [&] { return now; };
  test::DiscardEvents discard;
  md::RecordingSink sink(path, header, discard, options);
  std::vector<md::OptionContract> contracts;
  for (int i = 0; i < 7; ++i) {
    for (const auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
      auto contract = *md::parse_osi("SPXW261022C06000000");
      contract.strike = 5925 + i * 25;
      contract.type = type;
      sink.publish(md::ContractDefinition{static_cast<md::InstrumentId>(contracts.size()), contract});
      contracts.push_back(contract);
    }
  }
  for (int minute = 0; minute <= 390; minute += 5) {
    now = header.started + minute * md::kNanosPerMinute;
    const double spot = 6000 + minute * .01;
    sink.publish(md::UnderlyingQuote{"SPX", now, 0, 0, spot});
    for (std::size_t i = 0; i < contracts.size(); ++i) {
      const auto& c = contracts[i];
      const auto years = md::years_between(now, c.expiry_time());
      const auto mid = pricing::black_price(c.type, spot * std::exp(.027 * years), c.strike, years, .15, std::exp(-.04 * years));
      const auto bid = std::floor(mid * 10) / 10;
      sink.publish(md::OptionQuote{static_cast<md::InstrumentId>(i), now, bid, bid + .2, 20, 20});
    }
    sink.publish(md::ProviderStatus{now, md::FeedState::Live, "fixture", "SPX"});
  }
  sink.close();
}

TEST(ReplayHost, DrillsReachTheEngineBeforeTradingAndKeepTheirJournalsAcrossRestart) {
  using nlohmann::json;
  test::RecordingFile file;
  drill_recording(file.path);
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  std::string id;
  json expected;
  {
    server::ReplayHost host({file.directory, base, false});
    for (const auto* time : {"9:30", "24:00", "15:60", "garbage", "08:00", "17:00", ""}) {
      const json body{{"file", "session.oprec"}, {"start_at", time}};
      EXPECT_EQ(call(host, "POST", "/api/replay", body.dump()).status, 400) << time;
    }
    const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","start_at":"15:00","paused":true,"speed":60,"plan":"intraday-25k"})");
    ASSERT_EQ(started.status, 201) << started.body;
    const auto state = json::parse(started.body)["replay"];
    id = state["id"];
    EXPECT_TRUE(state["durable"]);
    EXPECT_NE(id.find("2026-09-16-recording-15-00"), std::string::npos);
    const json order{{"client_order_id", "drill"}, {"symbol", md::parse_osi("SPXW261022C06000000")->osi_symbol()},
                     {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}};
    const auto early = call(host, "POST", "/api/replay/orders", order.dump());
    ASSERT_EQ(early.status, 409) << early.body;
    EXPECT_EQ(json::parse(early.body)["error"]["code"], "REPLAY_FAST_FORWARD");
    ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(call(host, "GET", "/api/replay").body)["replay"]["fast_forwarding"].get<bool>(); }));
    const auto replay = json::parse(call(host, "GET", "/api/replay").body)["replay"];
    EXPECT_TRUE(replay["paused"]);
    EXPECT_EQ(replay["time"], "2026-09-16T19:00:00.000Z");
    EXPECT_EQ(replay["speed"], 60);
    const auto account = json::parse(call(host, "GET", "/api/replay/account").body);
    EXPECT_EQ(account["time"], "2026-09-16T19:00:00.000Z");
    const auto fills = json::parse(call(host, "GET", "/api/replay/fills").body);
    EXPECT_TRUE(fills["fills"].empty());
    const auto chain = json::parse(call(host, "GET", "/api/replay/underlyings/SPX/summary").body);
    EXPECT_EQ(chain["as_of"], "2026-09-16T19:00:00.000Z");
    const auto candles = json::parse(call(host, "GET", "/api/replay/underlyings/SPX/candles?interval=1m&limit=500").body);
    EXPECT_GT(candles["bars"].size(), 50U);
    const auto bought = call(host, "POST", "/api/replay/orders", order.dump());
    ASSERT_EQ(bought.status, 201) << bought.body;
    EXPECT_EQ(json::parse(bought.body)["order"]["status"], "filled");
    expected = json::parse(call(host, "GET", "/api/replay/fills").body);
    EXPECT_TRUE(json::parse(call(host, "GET", "/api/replay").body)["history"].empty());
    host.stop();
    const auto history = json::parse(call(host, "GET", "/api/replay").body)["history"];
    ASSERT_EQ(history.size(), 1U) << history;
    EXPECT_FALSE(history[0].contains("error")) << history;
    EXPECT_EQ(history[0]["result"], "open");
    EXPECT_EQ(history[0]["plan"], "intraday-25k");
    EXPECT_EQ(history[0]["plan_name"], "Intraday 25K");
    EXPECT_TRUE(history[0]["pnl"].is_string());
    EXPECT_FALSE(std::filesystem::exists(base.paper_journal));
    EXPECT_EQ(call(host, "POST", "/api/replay/history/" + id + "/orders", order.dump()).status, 403);
  }
  server::ReplayHost restarted({file.directory, base, false});
  const auto history = json::parse(call(restarted, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 1U);
  EXPECT_EQ(history[0]["id"], id);
  EXPECT_EQ(json::parse(call(restarted, "GET", "/api/replay/history/" + id + "/fills").body), expected);
  EXPECT_EQ(call(restarted, "GET", "/api/replay/history/" + id + "/account").status, 200);
  EXPECT_EQ(call(restarted, "GET", "/api/replay/history/" + id + "/trades").status, 200);
  EXPECT_EQ(call(restarted, "DELETE", "/api/replay/history/" + id).status, 200);
  EXPECT_TRUE(json::parse(call(restarted, "GET", "/api/replay").body)["history"].empty());
  EXPECT_EQ(call(restarted, "POST", "/api/replay", R"({"file":"session.oprec","paused":true})").status, 201);
  restarted.stop();
  const auto practice = json::parse(call(restarted, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(practice.size(), 1U);
  EXPECT_EQ(practice[0]["plan"], "practice");
  EXPECT_EQ(practice[0]["plan_name"], "Practice");
  EXPECT_FALSE(practice[0].contains("error")) << practice;
}

TEST(ReplayHost, AServerOfferingSandboxesStillStartsReplaysWithoutThem) {
  using nlohmann::json;
  test::RecordingFile file;
  drill_recording(file.path);
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  server::Sandboxes::Options limits;
  limits.capacity = 2;
  base.sandboxes = std::make_shared<server::Sandboxes>(limits);
  server::ReplayHost host({file.directory, base, false});
  const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","paused":true})");
  ASSERT_EQ(started.status, 201) << started.body;
  // Visitor sandboxes belong to the live demo: the replay neither advertises nor creates them.
  const auto status = call(host, "GET", "/api/replay/status");
  ASSERT_EQ(status.status, 200) << status.body;
  EXPECT_FALSE(json::parse(status.body).contains("sandboxes"));
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(call(host, "GET", "/api/replay").body)["replay"]["fast_forwarding"].get<bool>(); }));
  EXPECT_EQ(call(host, "POST", "/api/replay/sandboxes", "{}").status, 404);
  host.stop();
}

// B52, B32, B54: a delete takes a run's sidecars with it and works on a damaged
// journal; an unknown id is a 404 that names no file.
TEST(ReplayHost, HistoryDeletesDamagedRunsWithTheirSidecarsAndAnswersUnknownIds) {
  using nlohmann::json;
  test::RecordingFile file;
  drill_recording(file.path);
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  const auto replays = file.directory / "replays";
  server::ReplayHost host({file.directory, base, false});
  std::vector<std::string> ids;
  for (int run = 0; run < 3; ++run) {
    const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","start_at":"10:00","paused":true,"speed":0})");
    ASSERT_EQ(started.status, 201) << started.body;
    ids.push_back(json::parse(started.body)["replay"]["id"]);
    ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
    ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"10:30"})").status, 200);
    host.stop();
  }
  for (const auto& id : ids) {
    ASSERT_TRUE(std::filesystem::exists(replays / (id + ".json")));
    ASSERT_TRUE(std::filesystem::exists(replays / (id + ".jsonl.equity.csv"))) << id;
  }
  // An edited record breaks the chain; a torn last line is what a full disk leaves.
  const auto edited = replays / (ids[1] + ".jsonl");
  std::string lines;
  { std::ifstream in(edited); lines.assign(std::istreambuf_iterator<char>(in), {}); }
  const auto second = lines.find('\n') + 1;
  lines.replace(lines.find("\"seq\":2", second), 7, "\"seq\":9");
  { std::ofstream out(edited, std::ios::trunc); out << lines; }
  { std::ofstream out(replays / (ids[2] + ".jsonl"), std::ios::app); out << "{\"seq\":"; }
  const auto history = json::parse(call(host, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 3U);
  EXPECT_EQ(call(host, "GET", "/api/replay/history/" + ids[1]).status, 422);
  for (const auto& id : ids) {
    const auto deleted = call(host, "DELETE", "/api/replay/history/" + id);
    ASSERT_EQ(deleted.status, 200) << deleted.body;
    for (const auto* suffix : {".jsonl", ".json", ".jsonl.equity.csv", ".playbooks.json"})
      EXPECT_FALSE(std::filesystem::exists(replays / (id + suffix))) << id << suffix;
  }
  EXPECT_TRUE(json::parse(call(host, "GET", "/api/replay").body)["history"].empty());
  for (const auto* method : {"GET", "DELETE"}) {
    for (const auto* target : {"/api/replay/history/no-such-run", "/api/replay/history/no-such-run/fills", "/api/replay/history/..",
                               "/api/replay/history/"}) {
      const auto response = call(host, method, target);
      EXPECT_EQ(response.status, 404) << method << ' ' << target << ' ' << response.body;
      EXPECT_EQ(json::parse(response.body)["error"]["code"], "NOT_FOUND");
      EXPECT_EQ(response.body.find(file.directory.string()), std::string::npos) << response.body;
    }
  }
  // Another process's running run keeps its files.
  const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","paused":true})");
  ASSERT_EQ(started.status, 201) << started.body;
  const std::string running = json::parse(started.body)["replay"]["id"];
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
  server::Engine::Options other = base;
  server::ReplayHost second_host({file.directory, other, false});
  const auto refused = call(second_host, "DELETE", "/api/replay/history/" + running);
  EXPECT_EQ(refused.status, 409) << refused.body;
  EXPECT_EQ(json::parse(refused.body)["error"]["code"], "REPLAY_RUNNING");
  EXPECT_EQ(refused.body.find(file.directory.string()), std::string::npos) << refused.body;
  EXPECT_TRUE(std::filesystem::exists(replays / (running + ".jsonl")));
  host.stop();
}

// B53: a finished run's entry reports its final playback state and the plan's id,
// also when a crash left the metadata written at start.
TEST(ReplayHost, FinishedRunsListTheirFinalPlaybackStateAndPlanId) {
  using nlohmann::json;
  test::RecordingFile file;
  drill_recording(file.path);
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  server::ReplayHost host({file.directory, base, false});
  const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","start_at":"15:50","paused":true,"speed":0,"plan":"intraday-25k"})");
  ASSERT_EQ(started.status, 201) << started.body;
  const auto start_state = json::parse(started.body)["replay"];
  EXPECT_TRUE(start_state["fast_forwarding"]);
  const std::string id = start_state["id"];
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
  ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"15:55"})").status, 200);
  const auto check = [&](const json& entry) {
    EXPECT_EQ(entry["finished"], true) << entry;
    EXPECT_EQ(entry["read_only"], true);
    EXPECT_EQ(entry["fast_forwarding"], false);
    EXPECT_EQ(entry["progress"], 1.0);
    EXPECT_EQ(entry["paused"], true);
    EXPECT_EQ(entry["settled_through"], "2026-09-16T19:55:00.000Z");
    EXPECT_EQ(entry["plan"], "intraday-25k");
    EXPECT_EQ(entry["plan_name"], "Intraday 25K");
  };
  host.stop();
  auto history = json::parse(call(host, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 1U);
  check(history[0]);
  // A crash leaves the metadata create wrote at the start.
  { std::ofstream out(file.directory / "replays" / (id + ".json"), std::ios::trunc); out << start_state.dump() << '\n'; }
  server::ReplayHost restarted({file.directory, base, false});
  history = json::parse(call(restarted, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 1U);
  check(history[0]);
}

TEST(ReplayHost, PaperDisabledAndReadOnlyNeverCreateReplayJournals) {
  test::RecordingFile file;
  drill_recording(file.path);
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  base.paper_enabled = false;
  {
    server::ReplayHost host({file.directory, base, false});
    EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file":"session.oprec","paused":true})").status, 201);
    EXPECT_FALSE(std::filesystem::exists(file.directory / "replays"));
  }
  base.paper_enabled = true;
  base.write_mode = "disabled";
  server::ReplayHost host({file.directory, base, false});
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file":"session.oprec"})").status, 403);
  EXPECT_FALSE(std::filesystem::exists(file.directory / "replays"));
}


TEST(ReplayHost, FreshAndExplicitSeedsAndScenarioDrillsUseABoundedCache) {
  using nlohmann::json;
  test::RecordingFile file;
  server::Engine::Options base;
  server::ReplayHost host({file.directory, base});
  std::vector<std::string> fresh;
  for (int i = 0; i < 2; ++i) {
    const auto started = call(host, "POST", "/api/replay", R"({"scenario":"overnight","paused":true})", 60s);
    ASSERT_EQ(started.status, 201) << started.body;
    const auto state = json::parse(started.body)["replay"];
    fresh.push_back(state["seed"]);
    EXPECT_EQ(state["generator"], 1);
    EXPECT_EQ(state["scenario"], "overnight");
  }
  EXPECT_NE(fresh[0], fresh[1]);
  for (const auto* seed : {"0", "18446744073709551615", "scenario"}) {
    const json body{{"scenario", "overnight"}, {"seed", seed}, {"paused", true}};
    const auto started = call(host, "POST", "/api/replay", body.dump(), 60s);
    ASSERT_EQ(started.status, 201) << started.body;
    EXPECT_EQ(json::parse(started.body)["replay"]["seed"], std::string(seed) == "scenario" ? "2026091600" : seed);
  }
  const auto directories = demo_directories();
  ASSERT_EQ(directories.size(), 1U);
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directories.front()))
    if (entry.path().extension() == ".oprec") ++count;
  EXPECT_LE(count, 4U);
  for (const auto& seed : {json(-1), json(1.5), json("18446744073709551616"), json("fresh")})
    EXPECT_EQ(call(host, "POST", "/api/replay", json{{"scenario", "overnight"}, {"seed", seed}}.dump()).status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"scenario":"overnight","start_at":"19:00"})").status, 400);
  const auto started = call(host, "POST", "/api/replay", R"({"scenario":"overnight","seed":"scenario","start_at":"21:00","paused":true})", 60s);
  ASSERT_EQ(started.status, 201) << started.body;
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
  EXPECT_EQ(json::parse(host.tick())["replay"]["time"], "2026-09-16T01:00:00.000Z");
  EXPECT_TRUE(json::parse(host.tick())["replay"]["paused"]);
  // U3: scenario: true plays the default scenario, as demo: true does.
  const auto default_day = call(host, "POST", "/api/replay", R"({"scenario":true,"seed":"scenario","paused":true})", 60s);
  ASSERT_EQ(default_day.status, 201) << default_day.body;
  EXPECT_EQ(json::parse(default_day.body)["replay"]["scenario"], "reversal");
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"scenario":7})").status, 400);
}


TEST(ReplayHost, SavedRunSummariesOutliveTheFullAccountCache) {
  using nlohmann::json;
  test::RecordingFile file;
  const auto replays = file.directory / "replays";
  std::filesystem::create_directory(replays);
  test::ScriptedMarket market;
  trading::SessionConfig config;
  config.rules.plan = "Practice";
  for (int i = 0; i < 20; ++i) {
    const auto path = replays / ("run-" + std::to_string(i) + ".jsonl");
    const trading::TradingSession session(config, market.time, trading::FileJournal::create(path.string()));
  }
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  server::ReplayHost host({{}, options, false});
  const auto list = [&] { return json::parse(call(host, "GET", "/api/replay").body)["history"]; };
  const auto first = list();
  ASSERT_EQ(first.size(), 20U);
  for (const auto& item : first) {
    EXPECT_FALSE(item.contains("error")) << item;
    EXPECT_EQ(item["result"], "open");
    EXPECT_EQ(item["pnl"], "0.00");
    EXPECT_TRUE(item["valuation_complete"]);
    // Without a metadata sidecar the plan's id comes from its recorded name.
    EXPECT_EQ(item["plan"], "practice");
    EXPECT_EQ(item["plan_name"], "Practice");
    EXPECT_EQ(item["time"], md::format_timestamp(market.time));
  }
  EXPECT_EQ(host.history_recoveries(), 20U);
  EXPECT_EQ(list(), first);
  EXPECT_EQ(host.history_recoveries(), 20U);

  // Opening more full archives than fit in their cache must not evict summaries.
  for (int i = 0; i < 20; ++i)
    ASSERT_EQ(call(host, "GET", "/api/replay/history/run-" + std::to_string(i) + "/account").status, 200);
  EXPECT_EQ(host.history_recoveries(), 40U);
  EXPECT_EQ(list(), first);
  EXPECT_EQ(host.history_recoveries(), 40U);

  const auto changed = replays / "run-0.jsonl";
  const auto modified = std::filesystem::last_write_time(changed) + 1s;
  std::filesystem::last_write_time(changed, modified);
  EXPECT_EQ(list(), first);
  EXPECT_EQ(host.history_recoveries(), 41U);
  EXPECT_EQ(list(), first);
  EXPECT_EQ(host.history_recoveries(), 41U);

  std::string pnl;
  {
    auto session = trading::TradingSession::recover(trading::FileJournal::read(changed.string()),
                                                    trading::FileJournal::resume(changed.string()));
    market.next();
    market.seed(session);
    ASSERT_TRUE(session.submit(market.market("buy"), market.time).decision.ok());
    pnl = (session.snapshot()->equity - session.config().initial_cash).str();
  }
  // Preserve mtime to prove size changes alone invalidate a cached summary.
  std::filesystem::last_write_time(changed, modified);
  const auto updated = list();
  EXPECT_EQ(host.history_recoveries(), 42U);
  EXPECT_NE(updated, first);
  for (const auto& item : updated) if (item["id"] == "run-0") {
    EXPECT_EQ(item["pnl"], pnl);
    EXPECT_NE(item["pnl"], "0.00");
    EXPECT_EQ(item["time"], md::format_timestamp(market.time));
  }
  EXPECT_EQ(list(), updated);
  EXPECT_EQ(host.history_recoveries(), 42U);
  EXPECT_EQ(call(host, "DELETE", "/api/replay/history/run-0").status, 200);
  EXPECT_EQ(list().size(), 19U);
  EXPECT_EQ(host.history_recoveries(), 42U);
}

TEST(ReplayHost, OlderJournalRecordsNeedNoReplayFieldsAndStillCompactAndRepair) {
  using nlohmann::json;
  test::RecordingFile file;
  const auto replays = file.directory / "replays";
  std::filesystem::create_directory(replays);
  const auto journal = replays / "older.jsonl";
  std::string expected;
  {
    test::ScriptedMarket market;
    trading::TradingSession session({}, market.time, trading::FileJournal::create(file.path.string()));
    market.seed(session);
    ASSERT_TRUE(session.submit(market.market("buy"), market.time).decision.ok());
    expected = session.snapshot_json();
  }
  // Expand to the whole-state records older builds wrote; no replay sidecar.
  trading::TradingSession::expand(trading::FileJournal::read(file.path.string()), *trading::FileJournal::create(journal.string()));
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  server::ReplayHost host({{}, options, false});
  const auto history = json::parse(call(host, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 1U);
  EXPECT_FALSE(history[0].contains("error")) << history;
  EXPECT_EQ(history[0]["result"], "open");
  EXPECT_EQ(call(host, "GET", "/api/replay/history/older/account").status, 200);
  const auto compacted = server::compact_paper_journals(options.paper_journal, {});
  ASSERT_EQ(compacted.size(), 1U);
  EXPECT_TRUE(compacted[0].error.empty());
  EXPECT_FALSE(compacted[0].backup.empty());
  EXPECT_EQ(trading::TradingSession::recover(trading::FileJournal::read(journal.string())).snapshot_json(), expected);
  { std::ofstream tail(journal, std::ios::app); tail << "{torn"; }
  const auto repaired = trading::FileJournal::repair(journal.string());
  EXPECT_GT(repaired.bytes_cut, 0U);
  EXPECT_EQ(trading::TradingSession::recover(trading::FileJournal::read(journal.string())).snapshot_json(), expected);
  EXPECT_EQ(call(host, "GET", "/api/replay/history/older/fills").status, 200);
}

TEST(ReplayHost, FinishedRunsServeTheirEquityHistoryWithoutRewritingIt) {
  // B22: the archive recovered the account from its journal but never read the
  // equity history beside it, so a finished run's Dashboard had no chart.
  using nlohmann::json;
  test::RecordingFile file;
  const auto replays = file.directory / "replays";
  std::filesystem::create_directory(replays);
  const auto journal = replays / "finished.jsonl";
  test::ScriptedMarket market;
  {
    trading::TradingSession session({}, market.time, trading::FileJournal::create(journal.string()));
    market.seed(session);
    ASSERT_TRUE(session.submit(market.market("buy"), market.time).decision.ok());
  }
  const auto history = journal.string() + ".equity.csv";
  {
    server::EquityStore store(history);
    store.append({market.time, 1, trading::Money::parse("100000"), {}, trading::Money::parse("100000"), {}, {}, 0});
    store.append({market.time, 1, trading::Money::parse("99989.35"), {}, trading::Money::parse("100000"), {}, {}, 1});
    store.append({market.time + md::kNanosPerMinute, 1, trading::Money::parse("99979.35"), {}, trading::Money::parse("100000"), {}, {}, 0});
  }
  { std::ofstream torn(history, std::ios::app); torn << "torn"; }
  const auto bytes = std::filesystem::file_size(history);
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  server::ReplayHost host({{}, options, false});
  const auto response = call(host, "GET", "/api/replay/history/finished/account/equity");
  ASSERT_EQ(response.status, 200) << response.body;
  const auto body = json::parse(response.body);
  ASSERT_EQ(body["samples"].size(), 3U) << body;
  EXPECT_EQ(body["samples"][1]["equity"], "99989.35");
  EXPECT_EQ(body["samples"][1]["fill"], "1");
  EXPECT_EQ(body["samples"][2]["time"], md::format_timestamp(market.time + md::kNanosPerMinute));
  EXPECT_TRUE(body["error"].is_string());  // the torn row is reported, not repaired
  const auto bounded = json::parse(call(host, "GET", "/api/replay/history/finished/account/equity?from=" +
      md::format_timestamp(market.time + md::kNanosPerMinute)).body);
  EXPECT_EQ(bounded["samples"].size(), 1U);
  EXPECT_EQ(std::filesystem::file_size(history), bytes);
}

TEST(ReplayHost, ImportedHeaderIsListedAndRetainedInActiveReplay) {
  using namespace openport;
  using nlohmann::json;
  test::RecordingFile file;
  auto header = test::recording_header();
  header.provider = "thetadata";
  header.imported = true;
  header.started = md::new_york_to_utc({2026, 9, 22}, 9, 30);
  header.subscription = {{"SPY"}};
  header.capabilities.delay = std::chrono::seconds(0);
  header.capabilities.poll_interval = std::chrono::seconds(0);
  test::record_events(file.path,
      {md::ContractDefinition{0, *md::parse_osi("SPY260923C00500000")},
       md::OptionQuote{0, header.started, 5, 5.2, 10, 10}}, header,
      {.clock = [&] { return header.started; }});
  server::Engine::Options base;
  base.paper_enabled = false;
  server::ReplayHost host({file.directory, base, false});
  auto listing = json::parse(call(host, "GET", "/api/replay").body);
  ASSERT_EQ(listing["recordings"].size(), 1U);
  EXPECT_EQ(listing["recordings"][0]["imported"], true);
  EXPECT_EQ(listing["recordings"][0]["provider"], "thetadata");
  EXPECT_EQ(listing["recordings"][0]["simulated"], false);
  const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","speed":0})");
  ASSERT_EQ(started.status, 201) << started.body;
  EXPECT_EQ(json::parse(started.body)["replay"]["imported"], true);
  host.stop();
}

}  // namespace

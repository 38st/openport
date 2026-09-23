#include "support/recording.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "openport/providers/replay.hpp"
#include "openport/server/engine.hpp"
#include "openport/server/replay_host.hpp"

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

server::ApiResponse call(server::ReplayHost& host, std::string method, std::string target, std::string body = {},
                         std::chrono::seconds timeout = 5s) {
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
  ASSERT_EQ(bought.status, 201) << bought.body;
  EXPECT_EQ(json::parse(bought.body)["order"]["status"], "filled");
  EXPECT_EQ(json::parse(call(host, "GET", "/api/replay/portfolio").body)["positions"].size(), 1);

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

TEST(ReplayHost, PlaysTheSimulatedDemoMarketWithItsOwnAccount) {
  using nlohmann::json;
  test::RecordingFile file;
  server::Engine::Options base;
  base.analytics_interval = 1ms;
  server::ReplayHost host({file.directory, base});
  const auto listing = json::parse(call(host, "GET", "/api/replay").body);
  EXPECT_EQ(listing["demo"]["provider"], "demo");
  EXPECT_EQ(listing["demo"]["symbols"], json::array({"SPX", "SPY"}));
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"demo": true, "file": "synthetic.oprec"})").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", "{}").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"demo": 1})").status, 400);

  // Generated on start; at 1x the opening snapshot plays at once and the next waits 15 seconds.
  const auto started = call(host, "POST", "/api/replay", R"({"demo": true, "plan": "intraday-25k"})", 60s);
  ASSERT_EQ(started.status, 201) << started.body;
  const auto replay = json::parse(started.body)["replay"];
  EXPECT_EQ(replay["demo"], true);
  EXPECT_EQ(replay["file"], "Demo market");
  EXPECT_EQ(replay["provider"], "demo");
  // The player holds the generated file open; nothing is left on disk.
  const auto prefix = "openport-demo-" + std::to_string(::getpid()) + "-";
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::temp_directory_path()))
    EXPECT_FALSE(entry.path().filename().string().starts_with(prefix)) << entry.path();

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
}  // namespace

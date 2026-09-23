#include "openport/server/engine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <nlohmann/json.hpp>
#include <thread>

#include "openport/server/api.hpp"

namespace {

using namespace openport;

class ManualProvider final : public md::Provider {
 public:
  std::string_view name() const noexcept override { return "manual"; }
  md::Capabilities capabilities() const noexcept override { return caps; }
  void start(const md::Subscription&, md::EventSink& out) override { sink = &out; }
  void stop() override {}
  md::Capabilities caps;
  md::EventSink* sink = nullptr;
};

template <typename Predicate>
bool eventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

TEST(Engine, KeepsErrorsPerUnderlyingAndExpiresHealthUsingReceiptTime) {
  ManualProvider provider;
  provider.caps.poll_interval = std::chrono::seconds(30);
  std::atomic<md::Timestamp> clock{1000 * md::kNanosPerSecond};
  server::Engine::Options options;
  options.clock = [&] { return clock.load(); };
  server::Engine engine(provider, {{"SPX", "SPY"}}, options);
  engine.start();
  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Error, "SPX: offline", "SPX"});
  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Delayed, "SPY: delayed", "SPY"});
  ASSERT_TRUE(eventually([&] { return engine.status().underlyings.at("SPY").last_success > 0; }));
  auto status = engine.status();
  EXPECT_EQ(status.feed_state, md::FeedState::Error);
  EXPECT_EQ(status.underlyings.at("SPX").last_error, "SPX: offline");
  EXPECT_EQ(status.underlyings.at("SPY").state, md::FeedState::Delayed);
  EXPECT_EQ(status.underlyings.at("SPY").last_success, clock.load());
  clock += 90 * md::kNanosPerSecond;
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Delayed);
  clock += md::kNanosPerSecond;
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Stale);
  EXPECT_EQ(engine.status().feed_state, md::FeedState::Stale);

  const auto json = nlohmann::json::parse(server::handle_api({"GET", "/api/status"}, engine).body);
  ASSERT_EQ(json["underlyings"].size(), 2u);  // visible before any analytics exist
  EXPECT_EQ(json["underlyings"][0]["last_error"], "SPX: offline");
  EXPECT_EQ(json["underlyings"][1]["state"], "stale");
  EXPECT_TRUE(json["underlyings"][1]["spot"].is_null());
  EXPECT_EQ(json["feed"]["state"], "stale");
  const auto tick = nlohmann::json::parse(server::tick_message(engine));
  EXPECT_EQ(tick["feed"]["state"], "stale");
  EXPECT_EQ(tick["underlyings"][1]["state"], "stale");

  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Live, "SPX: recovered", "SPX"});
  ASSERT_TRUE(eventually(
      [&] { return engine.status().underlyings.at("SPX").state == md::FeedState::Live; }));
  EXPECT_EQ(engine.status().underlyings.at("SPX").last_error, "SPX: offline");
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Stale);
  engine.stop();
}

TEST(Engine, StreamingQuotesRefreshOnlyTheirUnderlyingWithASixtySecondMinimum) {
  ManualProvider provider;
  provider.caps.realtime = true;
  std::atomic<md::Timestamp> clock{1000 * md::kNanosPerSecond};
  server::Engine::Options options;
  options.analytics_interval = std::chrono::milliseconds(1);
  options.clock = [&] { return clock.load(); };
  server::Engine engine(provider, {{"SPX", "SPY"}}, options);
  engine.start();
  clock += 61 * md::kNanosPerSecond;
  EXPECT_EQ(engine.status().feed_state, md::FeedState::Stale);
  provider.sink->publish(md::ContractDefinition{0, *md::parse_osi("SPY991218C00500000")});
  provider.sink->publish(md::OptionQuote{0, 1, 5, 6, 1, 1});
  provider.sink->publish(md::ContractDefinition{1, *md::parse_osi("SPY1991218C00500000")});
  ASSERT_TRUE(eventually([&] { return engine.status().nonstandard_contracts == 1; }));
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Live);
  EXPECT_EQ(engine.status().underlyings.at("SPX").state, md::FeedState::Stale);
  clock += 60 * md::kNanosPerSecond;
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Live);
  clock += md::kNanosPerSecond;
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Stale);
  const auto json = nlohmann::json::parse(server::handle_api({"GET", "/api/status"}, engine).body);
  EXPECT_EQ(json["engine"]["nonstandard_contracts"], 1);
  engine.stop();
}

}  // namespace

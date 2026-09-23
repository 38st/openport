#include "openport/providers/replay.hpp"

#include <condition_variable>
#include <fstream>

#include "openport/providers/factory.hpp"
#include "support/recording.hpp"

namespace {
using namespace openport;
using namespace std::chrono_literals;

class TestClock final : public providers::ReplayClock {
 public:
  TimePoint now() override { return TimePoint{} + 100s; }
  bool wait_until(TimePoint deadline, const std::atomic<bool>& stop) override {
    std::unique_lock lock(mutex);
    if (deadlines.size() == block_after) wake.wait(lock, [&] { return stop.load(); });
    if (stop) return false;
    deadlines.push_back(deadline - now());
    return true;
  }
  void interrupt() override {
    const std::lock_guard lock(mutex);
    wake.notify_all();
  }
  std::vector<TimePoint::duration> waits() {
    const std::lock_guard lock(mutex);
    return deadlines;
  }
  std::size_t block_after = std::numeric_limits<std::size_t>::max();

 private:
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<TimePoint::duration> deadlines;
};

std::vector<md::Event> session() {
  return {md::ContractDefinition{0, *md::parse_osi("SPXW261022C05000000")},
          md::OptionQuote{0, 50, 100, 101, 10, 10},
          md::OptionTrade{0, 51, 100.5, 1},
          md::OpenInterest{0, 49, 120},
          md::VendorGreeks{0, 52, .25, .5, .2, .3, -.2, .1},
          md::UnderlyingQuote{"SPX", 53, 5000, 5001, 5000.5},
          md::ProviderStatus{54, md::FeedState::Delayed, "complete", "SPX"}};
}

bool ended(const test::EventCollector& sink) {
  const auto events = sink.snapshot();
  if (events.empty()) return false;
  const auto* status = std::get_if<md::ProviderStatus>(&events.back());
  return status &&
         (status->state == md::FeedState::Stopped || status->state == md::FeedState::Error);
}

TEST(Replay, PreservesOrderMarketTimeCapabilitiesAndAbsoluteTimingAtEverySpeed) {
  test::RecordingFile file;
  const auto events = session();
  md::RecordingSink::Options recording;
  md::Timestamp received = 100000;
  recording.clock = [&] {
    received += 60 * md::kNanosPerSecond;
    return received;
  };
  test::record_events(file.path, events, test::recording_header(), recording);
  for (const int speed : {1, 10, 60, 0}) {
    auto clock = std::make_shared<TestClock>();
    providers::ReplayProvider replay({file.path, speed, false, clock});
    EXPECT_EQ(replay.name(), "replay (synthetic)");
    const auto caps = replay.capabilities();
    EXPECT_EQ(caps.poll_interval, 15s);
    EXPECT_EQ(caps.delay, 900s);
    EXPECT_TRUE(caps.realtime && caps.realtime_plan_dependent && caps.quotes && caps.trades &&
                caps.open_interest && caps.vendor_greeks && caps.history);
    test::EventCollector sink;
    replay.start({{"SPX"}}, sink);
    ASSERT_TRUE(test::recording_eventually([&] { return ended(sink); }));
    replay.stop();
    const auto actual = sink.snapshot();
    ASSERT_EQ(actual.size(), events.size() + 1);
    for (std::size_t i = 0; i < events.size(); ++i) test::exact_event(events[i], actual[i]);
    const auto waits = clock->waits();
    ASSERT_EQ(waits.size(), speed == 0 ? 0u : events.size());
    for (std::size_t i = 0; i < waits.size(); ++i)
      EXPECT_EQ(waits[i], std::chrono::seconds(60 * static_cast<int>(i) / speed));
    EXPECT_EQ(std::get<md::ProviderStatus>(actual.back()).state, md::FeedState::Stopped);
  }
}

TEST(Replay, FiltersEverySymbolScopedEventAndKeepsGapsAcrossFilteredEvents) {
  test::RecordingFile file;
  std::vector<md::Event> events{md::ContractDefinition{0, *md::parse_osi("SPY261022C00500000")},
                                md::OptionQuote{0},
                                md::OptionTrade{0},
                                md::OpenInterest{0},
                                md::VendorGreeks{0},
                                md::UnderlyingQuote{"SPY"},
                                md::ProviderStatus{0, md::FeedState::Live, "SPY", "SPY"},
                                md::ContractDefinition{1, *md::parse_osi("SPXW261022C05000000")},
                                md::OptionQuote{1},
                                md::ProviderStatus{0, md::FeedState::Error, "global", ""},
                                md::ContractDefinition{1, *md::parse_osi("SPY261022C00500000")},
                                md::OptionQuote{1}};
  md::RecordingSink::Options recording;
  md::Timestamp receipt = 0;
  recording.clock = [&] {
    receipt += md::kNanosPerSecond;
    return receipt;
  };
  test::record_events(file.path, events, test::recording_header(), recording);
  auto clock = std::make_shared<TestClock>();
  providers::ReplayProvider replay({file.path, 1, false, clock});
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  ASSERT_TRUE(test::recording_eventually([&] { return sink.snapshot().size() == 4; }));
  replay.stop();
  const auto actual = sink.snapshot();
  ASSERT_EQ(actual.size(), 4u);
  for (std::size_t i = 0; i < 3; ++i) test::exact_event(events[7 + i], actual[i]);
  const auto waits = clock->waits();
  ASSERT_EQ(waits.size(), 3u);
  EXPECT_EQ(waits[0], 7s);
  EXPECT_EQ(waits[1], 8s);
  EXPECT_EQ(waits[2], 9s);
}

TEST(Replay, UnknownSymbolsFailSynchronouslyAndListAvailableSymbols) {
  test::RecordingFile file;
  test::record_events(file.path, session());
  providers::ReplayProvider replay({.file = file.path, .speed = 1, .loop = false, .clock = {}});
  test::EventCollector sink;
  try {
    replay.start({{"QQQ"}}, sink);
    FAIL();
  } catch (const std::invalid_argument& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("QQQ"), std::string::npos);
    EXPECT_NE(message.find("SPX, SPY"), std::string::npos);
  }
  EXPECT_TRUE(sink.snapshot().empty());
}

TEST(Replay, LoopsFromDefinitionsWithTheSameTimingAndStopsPromptly) {
  test::RecordingFile file;
  const auto events = session();
  md::RecordingSink::Options recording;
  md::Timestamp receipt = 0;
  recording.clock = [&] {
    receipt += md::kNanosPerSecond;
    return receipt;
  };
  test::record_events(file.path, events, test::recording_header(), recording);
  auto clock = std::make_shared<TestClock>();
  // One extra wait at each loop boundary preserves its trailing filtered gap.
  clock->block_after = 3 * (events.size() + 1);
  providers::ReplayProvider replay({file.path, 10, true, clock});
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  ASSERT_TRUE(
      test::recording_eventually([&] { return sink.snapshot().size() >= 3 * events.size(); }));
  replay.stop();
  const auto actual = sink.snapshot();
  ASSERT_EQ(actual.size(), 3 * events.size() + 1);
  for (std::size_t i = 0; i < 3 * events.size(); ++i)
    test::exact_event(events[i % events.size()], actual[i]);
  const auto waits = clock->waits();
  for (std::size_t i = 0; i < waits.size(); ++i)
    EXPECT_EQ(waits[i], 100ms * std::min(i % (events.size() + 1), events.size() - 1));
}

TEST(Replay, StopInterruptsHoursLongWaitAtEveryPacedSpeedAndMaxLoop) {
  test::RecordingFile file;
  md::RecordingSink::Options recording;
  md::Timestamp receipt = 0;
  recording.clock = [&] {
    receipt += md::kNanosPerDay;
    return receipt;
  };
  test::record_events(file.path, session(), test::recording_header(), recording);
  for (int speed : {1, 10, 60, 0}) {
    providers::ReplayProvider replay({.file = file.path, .speed = speed, .loop = true, .clock = {}});
    test::EventCollector sink;
    replay.start({{"SPX"}}, sink);
    ASSERT_TRUE(test::recording_eventually([&] { return !sink.snapshot().empty(); }));
    const auto started = std::chrono::steady_clock::now();
    replay.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_TRUE(ended(sink));
    replay.stop();
    EXPECT_THROW(replay.start({{"SPX"}}, sink), std::logic_error);
  }
}

TEST(Replay, TruncatedInputReportsRecoveryAndNeverLoopsIt) {
  test::RecordingFile file;
  test::record_events(file.path, session());
  std::filesystem::resize_file(file.path, std::filesystem::file_size(file.path) - 1);
  auto clock = std::make_shared<TestClock>();
  providers::ReplayProvider replay({file.path, 0, true, clock});
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  ASSERT_TRUE(test::recording_eventually([&] { return ended(sink); }));
  replay.stop();
  const auto events = sink.snapshot();
  ASSERT_EQ(events.size(), session().size() + 1);
  const auto& status = std::get<md::ProviderStatus>(events.back());
  EXPECT_EQ(status.state, md::FeedState::Stopped);
  EXPECT_NE(status.message.find("truncated"), std::string::npos);
}

TEST(Replay, EmptyFileTerminatesEvenWhenLoopingAndUndefinedContractsFailClosed) {
  for (bool undefined : {false, true}) {
    test::RecordingFile file;
    test::record_events(file.path, undefined ? std::vector<md::Event>{md::OptionQuote{42}}
                                             : std::vector<md::Event>{});
    providers::ReplayProvider replay({.file = file.path, .speed = 0, .loop = true, .clock = {}});
    test::EventCollector sink;
    replay.start({{"SPX"}}, sink);
    ASSERT_TRUE(test::recording_eventually([&] { return ended(sink); }));
    replay.stop();
    ASSERT_EQ(sink.snapshot().size(), 1u);
    EXPECT_EQ(std::get<md::ProviderStatus>(sink.snapshot()[0]).state,
              undefined ? md::FeedState::Error : md::FeedState::Stopped);
  }
}

TEST(Replay, BackwardAndExtremeReceiptTimesNeverOverflowOrReorder) {
  test::RecordingFile file;
  const std::vector<md::Timestamp> receipts{std::numeric_limits<md::Timestamp>::max(),
                                            std::numeric_limits<md::Timestamp>::min(),
                                            std::numeric_limits<md::Timestamp>::max()};
  std::size_t index = 0;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return receipts[index++]; };
  const std::vector<md::Event> events{md::UnderlyingQuote{"SPX", 1}, md::UnderlyingQuote{"SPX", 2},
                                      md::UnderlyingQuote{"SPX", 3}};
  test::record_events(file.path, events, test::recording_header(), recording);
  auto clock = std::make_shared<TestClock>();
  providers::ReplayProvider replay({file.path, 1, false, clock});
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  ASSERT_TRUE(test::recording_eventually([&] { return ended(sink); }));
  replay.stop();
  const auto waits = clock->waits();
  ASSERT_EQ(waits.size(), 3u);
  EXPECT_EQ(waits[0], 0s);
  EXPECT_EQ(waits[1], 0s);
  EXPECT_EQ(waits[2], providers::ReplayClock::TimePoint::max() - clock->now());
  for (std::size_t i = 0; i < events.size(); ++i) test::exact_event(events[i], sink.snapshot()[i]);
}

TEST(Replay, FactoryRejectsUnknownKeysBadValuesMissingFilesAndUnsupportedFilters) {
  test::RecordingFile file;
  test::record_events(file.path, {});
  EXPECT_THROW((void)providers::make_provider({"replay", "", {}}), std::invalid_argument);
  for (const auto& [key, value] :
       std::vector<std::pair<std::string, std::string>>{{"typo", "1"},
                                                        {"speed", "0"},
                                                        {"speed", "2"},
                                                        {"speed", "1x"},
                                                        {"speed", "nan"},
                                                        {"loop", "true"},
                                                        {"poll_seconds", "1"}})
    EXPECT_THROW((void)providers::make_provider(
                     {"replay", "", {{"file", file.path.string()}, {key, value}}}),
                 std::invalid_argument);
  EXPECT_THROW(
      (void)providers::make_provider({"replay", "", {{"file", "/missing/openport.oprec"}}}),
      std::runtime_error);
  auto provider = providers::make_provider(
      {"replay", "", {{"file", file.path.string()}, {"speed", "max"}, {"loop", "off"}}});
  EXPECT_EQ(provider->name(), "replay (synthetic)");
  EXPECT_THROW(providers::validate_subscription("replay", {{"SPX"}, 1, 0}), std::invalid_argument);
  EXPECT_THROW(providers::validate_subscription("replay", {{"SPX"}, 0, .1}), std::invalid_argument);
}
TEST(Replay, SpeedScalingCarriesFractionalNanosecondsWithoutAccumulatingDrift) {
  test::RecordingFile file;
  std::vector<md::Event> events(121, md::UnderlyingQuote{"SPX", 1});
  md::Timestamp receipt = 0;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return ++receipt; };
  test::record_events(file.path, events, test::recording_header(), recording);
  for (int speed : {10, 60}) {
    auto clock = std::make_shared<TestClock>();
    providers::ReplayProvider replay({file.path, speed, false, clock});
    test::EventCollector sink;
    replay.start({{"SPX"}}, sink);
    ASSERT_TRUE(test::recording_eventually([&] { return ended(sink); }));
    replay.stop();
    const auto waits = clock->waits();
    ASSERT_EQ(waits.size(), events.size());
    for (std::size_t i = 0; i < waits.size(); ++i)
      EXPECT_EQ(waits[i], std::chrono::nanoseconds(i / static_cast<std::size_t>(speed)));
  }
}


/// A clock that moves only when the test advances it; waits block until then.
class ManualClock final : public providers::ReplayClock {
 public:
  TimePoint now() override {
    const std::lock_guard lock(mutex);
    return current;
  }
  bool wait_until(TimePoint deadline, const std::atomic<bool>& stop) override {
    std::unique_lock lock(mutex);
    waiting = deadline;
    changed.notify_all();
    changed.wait(lock, [&] { return stop.load() || current >= deadline; });
    waiting.reset();
    return !stop.load();
  }
  void interrupt() override {
    const std::lock_guard lock(mutex);
    changed.notify_all();
  }
  void advance(TimePoint::duration by) {
    const std::lock_guard lock(mutex);
    current += by;
    changed.notify_all();
  }
  /// The deadline of the wait in progress, once one is.
  std::optional<TimePoint> waiting_for(std::optional<TimePoint> expected = {}) {
    std::unique_lock lock(mutex);
    changed.wait_for(lock, 3s, [&] { return waiting && (!expected || waiting == expected); });
    return waiting;
  }

 private:
  std::mutex mutex;
  std::condition_variable changed;
  TimePoint current{};
  std::optional<TimePoint> waiting;
};

/// Four events a minute apart, then one eight hours later: an overnight close.
test::RecordingFile paced_recording() {
  test::RecordingFile file;
  std::vector<md::Event> events = {md::ContractDefinition{0, *md::parse_osi("SPXW261022C05000000")},
                                   md::OptionQuote{0, 50, 100, 101, 10, 10}, md::OptionQuote{0, 51, 100, 101, 9, 9},
                                   md::OptionQuote{0, 52, 100, 101, 8, 8}, md::OptionQuote{0, 53, 100, 101, 7, 7}};
  md::RecordingSink::Options recording;
  md::Timestamp received = 1'000 * md::kNanosPerSecond;
  std::size_t index = 0;
  recording.clock = [&] {
    const auto now = received;
    received += index++ == 3 ? 8 * 3600 * md::kNanosPerSecond : 60 * md::kNanosPerSecond;
    return now;
  };
  test::record_events(file.path, events, test::recording_header(), recording);
  return file;
}

std::size_t quotes(const test::EventCollector& sink) {
  std::size_t count = 0;
  for (const auto& event : sink.snapshot()) count += std::holds_alternative<md::OptionQuote>(event);
  return count;
}

TEST(Replay, ASpeedChangeRescalesTheWaitInProgressAndThePausedTimeDoesNotCount) {
  const auto file = paced_recording();
  auto clock = std::make_shared<ManualClock>();
  providers::ReplayProvider replay({file.path, 1, false, clock});
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  const auto start = clock->now();
  // The first quote is a minute after the definition, at 1x.
  ASSERT_EQ(clock->waiting_for(start + 60s), start + 60s);
  EXPECT_EQ(quotes(sink), 0u);
  replay.set_speed(60);
  EXPECT_EQ(replay.speed(), 60);
  ASSERT_EQ(clock->waiting_for(start + 1s), start + 1s) << "a minute at 60x is a second";
  clock->advance(1s);
  ASSERT_TRUE(test::recording_eventually([&] { return quotes(sink) == 1; }));
  EXPECT_EQ(replay.time(), 1'060 * md::kNanosPerSecond);
  // The next quote is a minute of replay, a second of wall time at 60x; pause half way.
  ASSERT_EQ(clock->waiting_for(start + 2s), start + 2s);
  replay.set_paused(true);
  EXPECT_TRUE(replay.paused());
  clock->advance(10min);
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(quotes(sink), 1u) << "nothing plays while paused";
  replay.set_paused(false);
  // Ten paused minutes move the deadline back by ten minutes.
  ASSERT_EQ(clock->waiting_for(start + 2s + 10min), start + 2s + 10min);
  clock->advance(1s);
  ASSERT_TRUE(test::recording_eventually([&] { return quotes(sink) == 2; }));
  replay.stop();
}

TEST(Replay, SkipCutsAnOvernightGapShortAndTheEndIsReported) {
  const auto file = paced_recording();
  auto clock = std::make_shared<ManualClock>();
  providers::ReplayProvider replay({file.path, 60, false, clock});
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(clock->waiting_for().has_value());
    clock->advance(1s);
    ASSERT_TRUE(test::recording_eventually([&] { return quotes(sink) == static_cast<std::size_t>(i + 1); }));
  }
  // Eight hours at 60x is eight minutes; skip plays the next quote now.
  const auto now = clock->now();
  ASSERT_EQ(clock->waiting_for(now + 8min), now + 8min);
  replay.skip();
  ASSERT_TRUE(test::recording_eventually([&] { return quotes(sink) == 4; }));
  EXPECT_EQ(replay.time(), (1'000 + 180 + 8 * 3600) * md::kNanosPerSecond);
  ASSERT_TRUE(test::recording_eventually([&] { return replay.finished(); }));
  EXPECT_THROW(replay.set_speed(3), std::invalid_argument);
  replay.stop();
}
}  // namespace

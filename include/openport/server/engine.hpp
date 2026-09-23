#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "openport/analytics/chain_analytics.hpp"
#include "openport/analytics/chain_book.hpp"
#include "openport/md/event_queue.hpp"
#include "openport/md/provider.hpp"
#include "openport/md/recording.hpp"
#include "openport/server/paper.hpp"

namespace openport::server {

struct UnderlyingHealth {
  md::FeedState state = md::FeedState::Connecting;
  std::string message;
  md::Timestamp last_success = 0;  ///< local receipt time, never the delayed market-data clock
  std::string last_error;
  md::Timestamp last_error_time = 0;
};

/// A consistent picture of the feed and the engine for the status endpoint.
struct EngineStatus {
  TradingStatus trading;
  std::string provider;
  md::Capabilities capabilities;
  md::FeedState feed_state = md::FeedState::Connecting;
  std::string feed_message;
  md::Timestamp feed_updated = 0;
  std::size_t queue_depth = 0;
  std::uint64_t coalesced_events = 0;
  std::uint64_t dropped_events = 0;
  bool overloaded = false;
  std::uint64_t events = 0;
  double events_per_second = 0.0;
  double analytics_ms = 0.0;  ///< time the last analytics pass took
  std::size_t contracts = 0;
  std::size_t nonstandard_contracts = 0;
  md::Timestamp started = 0;
  std::map<std::string, UnderlyingHealth> underlyings;
};

/// Read-only view of the engine's results. The HTTP API depends only on this, so it
/// can be tested without a provider or a thread.
class MetricsSource {
 public:
  virtual ~MetricsSource() = default;
  [[nodiscard]] virtual std::vector<std::string> symbols() const = 0;
  /// nullptr until the first analytics pass for `symbol` has run.
  [[nodiscard]] virtual std::shared_ptr<const analytics::UnderlyingMetrics> metrics(
      const std::string& symbol) const = 0;
  [[nodiscard]] virtual EngineStatus status() const = 0;
  [[nodiscard]] virtual std::shared_ptr<const TradingView> trading_view() const { return {}; }
  /// False means unavailable or full. Completion runs on the engine thread;
  /// network callers must dispatch it onto their own executor.
  virtual bool post_trading(TradingCommand, TradingCompletion) { return false; }
};

/// Owns the provider's event stream: applies every event to the chain book on one
/// thread, and re-runs the analytics at a fixed cadence for each underlying whose
/// data changed. Results are published as immutable snapshots, so readers on other
/// threads never block the engine for longer than a pointer copy.
class Engine final : public MetricsSource {
 public:
  struct Options {
    std::chrono::milliseconds analytics_interval{1000};
    bool paper_enabled = true;
    std::filesystem::path paper_journal;  ///< Empty only for explicit in-process simulations.
    trading::SessionConfig paper;
    std::shared_ptr<trading::Journal> paper_sink;  ///< Optional in-process test/simulation sink.
    std::size_t command_capacity = 256;
    std::string write_mode = "open";
    analytics::AnalyticsOptions analytics;
    std::filesystem::path record_file;
    md::RecordingSink::Options recording;
    std::function<md::Timestamp()> clock = md::now;
    /// Monotonic cadence for publishing receipt timestamps, independent of wall-clock jumps.
    std::function<std::chrono::steady_clock::time_point()> monotonic_clock =
        std::chrono::steady_clock::now;
    /// Injectable so failure to create the consumer thread can be tested.
    std::function<std::thread(std::function<void()>)> launch = [](std::function<void()> run) {
      return std::thread(std::move(run));
    };
  };

  Engine(md::Provider& provider, md::Subscription subscription, Options options);
  ~Engine() override;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /// One attempt per instance; construct a new Engine to restart.
  /// Returns after paper journal initialization; failures are reported in status.
  void start();
  void stop();

  [[nodiscard]] std::vector<std::string> symbols() const override;
  [[nodiscard]] std::shared_ptr<const analytics::UnderlyingMetrics> metrics(
      const std::string& symbol) const override;
  [[nodiscard]] EngineStatus status() const override;
  [[nodiscard]] std::shared_ptr<const TradingView> trading_view() const override;
  bool post_trading(TradingCommand command, TradingCompletion completion) override;
  [[nodiscard]] md::RecordingStats recording_stats() const;
  [[nodiscard]] std::string recording_error() const;

 private:
  struct PendingCommand {
    std::uint64_t sequence;
    TradingCommand command;
    TradingCompletion completion;
  };
  void run();
  void start_trading();
  void observe_trading(const md::Event& event);
  void update_trading(const std::vector<md::Event>& batch, std::deque<PendingCommand>& commands);
  void apply_command(PendingCommand& pending);
  void publish_trading();
  void fail_trading(std::string reason);
  void refresh_analytics();
  void update_health(const md::Event& event, md::Timestamp received);

  md::Provider& provider_;
  md::Subscription subscription_;
  Options options_;
  md::EventQueue queue_;
  std::unique_ptr<md::RecordingSink> recorder_;
  analytics::ChainBook book_;  // engine thread only

  std::thread thread_;
  std::atomic<bool> stopping_{false};
  bool started_ = false;
  bool provider_started_ = false;

  mutable std::mutex mutex_;  // guards everything below
  std::map<std::string, std::shared_ptr<const analytics::UnderlyingMetrics>> metrics_;
  EngineStatus status_;
  std::shared_ptr<const TradingView> trading_view_;

  std::mutex command_mutex_;
  std::deque<PendingCommand> commands_;
  std::uint64_t next_command_ = 1;
  bool accepting_commands_ = false;

  // Initialized/recovered by start(), then owned exclusively by the engine thread.
  std::unique_ptr<trading::TradingSession> trading_;
  std::string trading_failure_;
  std::map<std::string, std::string> settlement_source_;
  md::Timestamp market_time_ = 0;
  std::map<std::string, md::InstrumentId> instruments_;
  std::map<std::string, std::uint64_t> observations_;

  // Engine thread only: quote receipt never locks the reader-facing status mutex.
  std::map<std::string, UnderlyingHealth> health_;
  md::Timestamp feed_updated_ = 0;
  bool health_dirty_ = false;
  bool health_changed_ = false;
  std::map<std::string, std::uint64_t> analysed_versions_;
  std::map<std::string, std::shared_ptr<const analytics::DiscountCurve>> discount_curves_;
  std::shared_ptr<const analytics::DiscountCurve> discount_curve_;
  std::uint64_t events_ = 0;
  std::uint64_t events_at_last_rate_ = 0;
  std::chrono::steady_clock::time_point last_rate_time_;
};

}  // namespace openport::server

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
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

namespace openport::server {

/// A consistent picture of the feed and the engine for the status endpoint.
struct EngineStatus {
  std::string provider;
  md::Capabilities capabilities;
  md::FeedState feed_state = md::FeedState::Connecting;
  std::string feed_message;
  md::Timestamp feed_updated = 0;
  std::uint64_t events = 0;
  double events_per_second = 0.0;
  double analytics_ms = 0.0;  ///< time the last analytics pass took
  std::size_t contracts = 0;
  md::Timestamp started = 0;
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
};

/// Owns the provider's event stream: applies every event to the chain book on one
/// thread, and re-runs the analytics at a fixed cadence for each underlying whose
/// data changed. Results are published as immutable snapshots, so readers on other
/// threads never block the engine for longer than a pointer copy.
class Engine final : public MetricsSource {
 public:
  struct Options {
    std::chrono::milliseconds analytics_interval{1000};
    analytics::AnalyticsOptions analytics;
  };

  Engine(md::Provider& provider, md::Subscription subscription, Options options);
  ~Engine() override;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void start();
  void stop();

  [[nodiscard]] std::vector<std::string> symbols() const override;
  [[nodiscard]] std::shared_ptr<const analytics::UnderlyingMetrics> metrics(
      const std::string& symbol) const override;
  [[nodiscard]] EngineStatus status() const override;

 private:
  void run();
  void refresh_analytics();

  md::Provider& provider_;
  md::Subscription subscription_;
  Options options_;
  md::EventQueue queue_;
  analytics::ChainBook book_;  // engine thread only

  std::thread thread_;
  std::atomic<bool> stopping_{false};

  mutable std::mutex mutex_;  // guards everything below
  std::map<std::string, std::shared_ptr<const analytics::UnderlyingMetrics>> metrics_;
  EngineStatus status_;

  // Engine thread only.
  std::map<std::string, std::uint64_t> analysed_versions_;
  std::uint64_t events_ = 0;
  std::uint64_t events_at_last_rate_ = 0;
  std::chrono::steady_clock::time_point last_rate_time_;
};

}  // namespace openport::server

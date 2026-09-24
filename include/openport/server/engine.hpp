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
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "openport/analytics/chain_analytics.hpp"
#include "openport/analytics/chain_book.hpp"
#include "openport/md/event_queue.hpp"
#include "openport/md/provider.hpp"
#include "openport/md/recording.hpp"
#include "openport/server/candles.hpp"
#include "openport/server/paper.hpp"
#include "openport/trading/dividends.hpp"

namespace openport::server {

struct UnderlyingHealth {
  md::FeedState state = md::FeedState::Connecting;
  std::string message;
  md::Timestamp last_success = 0;  ///< local receipt time, never the delayed market-data clock
  std::string last_error;
  md::Timestamp last_error_time = 0;
};

/// The main account keeps the original journal; others are named alongside it.
inline constexpr std::string_view kMainAccount = "main";

/// One paper account's standing, for the account list and ticks.
struct AccountStatus {
  std::string id;
  std::string name;
  TradingStatus trading;
};

/// A consistent picture of the feed and the engine for the status endpoint.
struct EngineStatus {
  TradingStatus trading;  ///< The main account's.
  std::vector<AccountStatus> accounts;  ///< Every account, the main one first.
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
  [[nodiscard]] virtual md::Timestamp wall_time() const { return md::now(); }
  /// The main account's publication.
  [[nodiscard]] virtual std::shared_ptr<const TradingView> trading_view() const { return {}; }
  /// A named account's publication; the main account's for an empty id. Null when unknown.
  [[nodiscard]] virtual std::shared_ptr<const TradingView> trading_view(std::string_view account) const {
    return account.empty() || account == kMainAccount ? trading_view() : nullptr;
  }
  /// Price history for charts; nullptr when this source keeps none.
  [[nodiscard]] virtual const CandleStore* candles() const { return nullptr; }
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
    std::filesystem::path paper_journal;  ///< The main account. Empty only for explicit in-process simulations.
    /// More named accounts, one journal each (<id>.jsonl, named in <id>.name). Empty for none.
    std::filesystem::path paper_accounts;
    trading::SessionConfig paper;
    /// Dividends every account pays on held shares at the rollover into each ex-date.
    std::vector<trading::Dividend> dividends;
    std::shared_ptr<trading::Journal> paper_sink;  ///< Optional in-process test/simulation sink.
    std::size_t command_capacity = 256;
    std::string write_mode = "open";
    analytics::AnalyticsOptions analytics;
    std::filesystem::path record_file;
    md::RecordingSink::Options recording;
    /// Receives the spot of every analysis, stamped with the time of its price.
    std::shared_ptr<CandleStore> candles;
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
  /// Replaces the dividends day rollovers pay (Options::dividends at first), as a
  /// fetcher learns of new ones. Thread-safe.
  void set_dividends(std::vector<trading::Dividend> dividends);

  [[nodiscard]] std::vector<std::string> symbols() const override;
  [[nodiscard]] std::shared_ptr<const analytics::UnderlyingMetrics> metrics(
      const std::string& symbol) const override;
  [[nodiscard]] EngineStatus status() const override;
  [[nodiscard]] md::Timestamp wall_time() const override { return options_.clock(); }
  [[nodiscard]] std::shared_ptr<const TradingView> trading_view() const override;
  [[nodiscard]] std::shared_ptr<const TradingView> trading_view(std::string_view account) const override;
  [[nodiscard]] const CandleStore* candles() const override { return options_.candles.get(); }
  bool post_trading(TradingCommand command, TradingCompletion completion) override;
  [[nodiscard]] md::RecordingStats recording_stats() const;
  [[nodiscard]] std::string recording_error() const;

 private:
  struct PendingCommand {
    std::uint64_t sequence;
    TradingCommand command;
    TradingCompletion completion;
  };
  /// One paper account: a reducer and its journal. Engine thread only.
  struct PaperAccount {
    std::string id;
    std::string name;
    std::unique_ptr<trading::TradingSession> session;
    std::string failure;  ///< Why it cannot trade; empty while it can.
  };
  void run();
  void start_trading();
  PaperAccount* find_account(std::string_view id);
  void create_account(const TradingCommand& command, TradingReply& reply);
  void observe_trading(const md::Event& event);
  void update_trading(const std::vector<md::Event>& batch, std::deque<PendingCommand>& commands);
  void apply_command(PendingCommand& pending);
  void publish_trading();
  /// Stops one account after a journal or integration failure; the others carry on.
  void fail_trading(PaperAccount& account, std::string reason);
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
  std::map<std::string, std::shared_ptr<const TradingView>, std::less<>> trading_views_;

  std::mutex command_mutex_;
  std::deque<PendingCommand> commands_;
  std::uint64_t next_command_ = 1;
  bool accepting_commands_ = false;

  // Initialized/recovered by start(), then owned exclusively by the engine thread.
  std::vector<PaperAccount> accounts_;  // the main account first
  std::map<std::string, std::string> settlement_source_;
  /// Each underlying's first print at or after a date's regular close, and its
  /// last one before it, for the last week of dates.
  std::map<std::pair<std::string, md::Date>, md::UnderlyingQuote> closing_prints_;
  std::map<std::pair<std::string, md::Date>, md::UnderlyingQuote> before_close_;
  /// Official closes by symbol and date (md::UnderlyingClose), for a week of dates.
  std::map<std::pair<std::string, md::Date>, md::UnderlyingClose> official_closes_;
  /// Circuit-breaker halts of the last day, and the level tripped on breaker_day_.
  std::vector<MarketHalt> halts_;
  md::Date breaker_day_;
  int breaker_level_ = 0;
  mutable std::mutex dividends_mutex_;
  std::vector<trading::Dividend> dividends_;
  void check_circuit_breaker(const md::UnderlyingQuote& spot);
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

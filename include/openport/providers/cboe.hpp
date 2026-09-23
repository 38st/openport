#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "openport/md/bars.hpp"
#include "openport/providers/snapshot.hpp"

namespace openport::providers {

/// One option row from a Cboe delayed-quotes snapshot, in Cboe's units.
struct CboeOption {
  std::string symbol;  ///< compact OSI, e.g. "SPXW261005P07405000"
  double bid = 0.0;
  double bid_size = 0.0;
  double ask = 0.0;
  double ask_size = 0.0;
  double iv = 0.0;  ///< decimal; 0 when Cboe has none
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double theta = 0.0;
  double rho = 0.0;
  double theo = 0.0;
  double open_interest = 0.0;
  double volume = 0.0;
};

/// A whole chain as Cboe serves it.
struct CboeChain {
  std::string symbol;       ///< as reported, e.g. "^SPX" or "SPY"
  md::Timestamp as_of = 0;  ///< when Cboe generated the snapshot (UTC)
  double price = 0.0;
  double bid = 0.0;
  double ask = 0.0;
  std::vector<CboeOption> options;
  md::Timestamp last_trade_time = 0;  ///< underlying print clock, independent of option sessions
};

/// Parses cdn.cboe.com/api/global/delayed_quotes/options/<symbol>.json.
/// Throws std::runtime_error if the document does not have the expected shape.
[[nodiscard]] CboeChain parse_cboe_chain(std::string_view json);

/// Chain URL for an underlying. Index symbols take a leading underscore (_SPX).
[[nodiscard]] std::string cboe_chain_url(std::string_view underlying);

/// Cboe's delayed chart files behind its quote pages: the latest regular session's
/// one-minute bars, and daily bars since the index or fund began.
enum class CboeChart : std::uint8_t { Intraday, Daily };

[[nodiscard]] std::string cboe_chart_url(std::string_view underlying, CboeChart chart);

/// One-minute bars of the latest regular session. Cboe labels a bar with the New
/// York minute it closes; each result starts one minute earlier. Rows that are not
/// valid bars are skipped. Throws std::runtime_error without a data array.
[[nodiscard]] std::vector<md::Bar> parse_cboe_intraday(std::string_view json);

/// Daily bars, oldest first, each starting at its session's 09:30 ET open. Cboe's
/// oldest index history has no open; rows that are not valid bars are skipped.
/// Throws std::runtime_error without a data array.
[[nodiscard]] std::vector<md::Bar> parse_cboe_daily(std::string_view json);

/// Keeps chart history current from Cboe's free chart files on its own thread: the
/// latest session's minute bars every minute while a regular session is open or
/// just closed (every 15 minutes otherwise), and daily bars every hour. Files Cboe
/// does not have (403/404) are retried hourly.
class CboeChartHistory {
 public:
  using Sink = std::function<void(const std::string& underlying, CboeChart chart,
                                  std::vector<md::Bar> bars)>;
  struct Options {
    std::chrono::seconds session_interval{60};
    std::chrono::seconds idle_interval{900};
    std::chrono::seconds daily_interval{3600};
    std::chrono::seconds missing_interval{3600};
    std::chrono::seconds timeout{20};
    std::function<md::Timestamp()> clock = md::now;
  };

  CboeChartHistory(std::vector<std::string> underlyings, Sink sink)
      : CboeChartHistory(std::move(underlyings), std::move(sink), Options{}) {}
  CboeChartHistory(std::vector<std::string> underlyings, Sink sink, Options options);
  ~CboeChartHistory() { stop(); }
  CboeChartHistory(const CboeChartHistory&) = delete;
  CboeChartHistory& operator=(const CboeChartHistory&) = delete;

  void start();
  /// Interrupts a request in flight and joins.
  void stop();

  /// Fetches every chart that is due, on the caller's thread. Returns when the next
  /// one falls due.
  md::Timestamp poll_once(net::HttpClient& http);

  /// The latest failure of each chart that is failing, one per line; empty when none is.
  [[nodiscard]] std::string error() const;

 private:
  void run();

  std::vector<std::string> underlyings_;
  Sink sink_;
  Options options_;
  std::map<std::pair<std::string, CboeChart>, md::Timestamp> due_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::mutex wake_mutex_;
  std::condition_variable wake_;
  mutable std::mutex error_mutex_;
  std::map<std::pair<std::string, CboeChart>, std::string> errors_;
};

/// Cboe's free delayed option quotes. No account or key needed; the data runs
/// 15 minutes behind the market and includes Cboe's own IV and Greeks.
class CboeDelayedProvider final : public PollingProvider {
 public:
  struct Options {
    std::chrono::seconds poll_interval{15};
    std::chrono::seconds timeout{30};
  };

  static constexpr std::chrono::minutes kDelay{15};

  CboeDelayedProvider() : CboeDelayedProvider(Options{}) {}
  explicit CboeDelayedProvider(Options options)
      : PollingProvider(options.poll_interval), options_(options) {}
  ~CboeDelayedProvider() override { stop(); }

  [[nodiscard]] std::string_view name() const noexcept override { return "cboe"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;

  /// Turns one parsed chain into events, publishing only what changed since the
  /// previous call.
  void publish_chain(const CboeChain& chain, const md::Subscription& subscription,
                     md::EventSink& sink);

 protected:
  std::string poll(net::HttpClient& http, const std::string& underlying,
                   const md::Subscription& subscription, md::EventSink& sink) override;
  [[nodiscard]] md::FeedState healthy_state() const noexcept override {
    return md::FeedState::Delayed;
  }

 private:
  Options options_;
  SnapshotPublisher publisher_;
};

}  // namespace openport::providers

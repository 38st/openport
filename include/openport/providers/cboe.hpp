#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "openport/md/provider.hpp"

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
  std::string symbol;    ///< as reported, e.g. "^SPX" or "SPY"
  md::Timestamp as_of = 0;  ///< when Cboe generated the snapshot (UTC)
  double price = 0.0;
  double bid = 0.0;
  double ask = 0.0;
  std::vector<CboeOption> options;
};

/// Parses cdn.cboe.com/api/global/delayed_quotes/options/<symbol>.json.
/// Throws std::runtime_error if the document does not have the expected shape.
[[nodiscard]] CboeChain parse_cboe_chain(std::string_view json);

/// Chain URL for an underlying. Index symbols take a leading underscore (_SPX).
[[nodiscard]] std::string cboe_chain_url(std::string_view underlying);

/// Cboe's free delayed option quotes. No account or key needed; the data runs
/// 15 minutes behind the market and includes Cboe's own IV and Greeks.
class CboeDelayedProvider final : public md::Provider {
 public:
  struct Options {
    std::chrono::seconds poll_interval{15};
    std::chrono::seconds timeout{30};
  };

  CboeDelayedProvider();
  explicit CboeDelayedProvider(Options options);
  ~CboeDelayedProvider() override;

  [[nodiscard]] std::string_view name() const noexcept override { return "cboe"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;

  void start(const md::Subscription& subscription, md::EventSink& sink) override;
  void stop() override;

  /// Turns one parsed chain into events, publishing only what changed since the
  /// previous call. Used by the polling thread; exposed for tests and tools.
  void publish_chain(const CboeChain& chain, const md::Subscription& subscription,
                     md::EventSink& sink);

  static constexpr std::chrono::minutes kDelay{15};

 private:
  struct Published {
    double bid = -1.0;
    double ask = -1.0;
    double bid_size = -1.0;
    double ask_size = -1.0;
    double open_interest = -1.0;
    double iv = -1.0;
    double delta = -2.0;
  };

  void run(md::Subscription subscription, md::EventSink* sink);
  bool sleep_for(std::chrono::seconds duration);

  Options options_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::mutex wake_mutex_;
  std::condition_variable wake_;

  std::unordered_map<std::string, md::InstrumentId> ids_;
  std::vector<Published> published_;
};

}  // namespace openport::providers

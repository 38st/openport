#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

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
  md::Timestamp last_trade_time = 0;  ///< New York market clock; caps frozen after-hours quotes
};

/// Parses cdn.cboe.com/api/global/delayed_quotes/options/<symbol>.json.
/// Throws std::runtime_error if the document does not have the expected shape.
[[nodiscard]] CboeChain parse_cboe_chain(std::string_view json);

/// Chain URL for an underlying. Index symbols take a leading underscore (_SPX).
[[nodiscard]] std::string cboe_chain_url(std::string_view underlying);

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

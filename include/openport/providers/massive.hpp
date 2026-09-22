#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "openport/providers/snapshot.hpp"

namespace openport::providers {

/// One contract from Massive's option chain snapshot.
struct MassiveContract {
  std::string symbol;  ///< compact OSI, "O:" prefix removed
  bool european = false;
  double multiplier = 100.0;

  bool has_quote = false;
  double bid = 0.0;
  double ask = 0.0;
  double bid_size = 0.0;
  double ask_size = 0.0;
  md::Timestamp quote_ts = 0;
  bool realtime = false;  ///< the quote's timeframe is "REAL-TIME" rather than "DELAYED"

  bool has_open_interest = false;
  double open_interest = 0.0;

  double iv = 0.0;  ///< decimal; 0 when Massive returns none (e.g. deep in the money)
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;   ///< per vol point
  double theta = 0.0;  ///< per calendar day
};

/// One page of `GET /v3/snapshot/options/{underlying}`.
struct MassivePage {
  std::vector<MassiveContract> contracts;
  std::string next_url;  ///< empty on the last page
  double underlying_price = 0.0;
  md::Timestamp underlying_ts = 0;
};

/// Parses one page of Massive's option chain snapshot. Throws std::runtime_error on
/// an error response or a document of the wrong shape.
[[nodiscard]] MassivePage parse_massive_chain_page(std::string_view json);

/// First-page URL for an underlying; indexes use Massive's "I:" prefix (I:SPX).
[[nodiscard]] std::string massive_chain_url(std::string_view base_url, std::string_view underlying);

/// Massive (formerly Polygon.io), using your own API key. Real-time or 15-minute
/// delayed depending on your plan; the feed status says which. Polls the whole chain
/// snapshot, so plans with per-minute request limits are too slow for large chains.
class MassiveProvider final : public PollingProvider {
 public:
  struct Options {
    std::string api_key;
    std::string base_url = "https://api.massive.com";
    std::chrono::seconds poll_interval{5};
    std::chrono::seconds timeout{30};
  };

  explicit MassiveProvider(Options options);
  ~MassiveProvider() override { stop(); }

  [[nodiscard]] std::string_view name() const noexcept override { return "massive"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;

  /// Publishes one complete chain (all pages of one poll).
  void publish_chain(const std::string& underlying, const std::vector<MassiveContract>& contracts,
                     double underlying_price, md::Timestamp underlying_ts,
                     const md::Subscription& subscription, md::EventSink& sink);

 protected:
  std::string poll(net::HttpClient& http, const std::string& underlying,
                   const md::Subscription& subscription, md::EventSink& sink) override;
  [[nodiscard]] md::FeedState healthy_state() const noexcept override {
    return realtime_ ? md::FeedState::Live : md::FeedState::Delayed;
  }

 private:
  Options options_;
  SnapshotPublisher publisher_;
  std::atomic<bool> realtime_{false};
};

}  // namespace openport::providers

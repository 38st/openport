#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "openport/net/http.hpp"
#include "openport/providers/dxlink.hpp"
#include "openport/providers/snapshot.hpp"

namespace openport::providers {
struct TastytradeContract {
  md::OptionContract contract;
  std::string streamer_symbol;
};
[[nodiscard]] std::vector<TastytradeContract> parse_tastytrade_chain(std::string_view json, std::string_view underlying);
[[nodiscard]] std::string parse_tastytrade_streamer(std::string_view json);

class TastytradeApi {
 public:
  struct Credentials { std::string client_secret, refresh_token, client_id; };
  explicit TastytradeApi(Credentials credentials);
  /// Only allowlisted market-data GETs. Auth POSTs are internal and never redirect.
  std::string get(net::HttpClient& http, const std::string& path, md::Timestamp now,
                  const std::atomic<bool>* cancel = nullptr);
  void refresh(net::HttpClient& http, md::Timestamp now, const std::atomic<bool>* cancel = nullptr);
  [[nodiscard]] md::Timestamp refresh_at() const noexcept { return refresh_at_; }

 private:
  Credentials credentials_;
  std::string access_token_;
  md::Timestamp refresh_at_ = 0;
};

struct TastytradeQuoteToken {
  std::string token, url;
  md::Timestamp refresh_at = 0;
};
[[nodiscard]] TastytradeQuoteToken parse_tastytrade_quote_token(std::string_view json, md::Timestamp now);

/// Definitions and event mapping are separate from protocol and transport.
class TastytradeMapper {
 public:
  TastytradeMapper(md::Subscription subscription, md::EventSink& sink, bool milliseconds = false)
      : subscription_(std::move(subscription)), sink_(sink), milliseconds_(milliseconds) {}
  void chain(std::string underlying, std::string streamer, std::vector<TastytradeContract> contracts, md::Date today);
  void event(const nlohmann::json& event);
  void reset_health() { live_.clear(); }
  [[nodiscard]] std::set<DxlinkSubscription> subscriptions() const;

 private:
  void select(const std::string& underlying);
  struct Chain { std::vector<TastytradeContract> contracts; md::Date today; double spot = 0; };
  md::Subscription subscription_;
  md::EventSink& sink_;
  bool milliseconds_;
  SnapshotPublisher definitions_;
  std::map<std::string, Chain> chains_;
  std::map<std::string, std::string> underlyings_;
  std::map<std::string, std::pair<md::InstrumentId, std::string>> instruments_;
  std::map<std::string, md::FeedState> live_;
};

class TastytradeProvider final : public md::Provider {
 public:
  struct Options {
    TastytradeApi::Credentials credentials;
    bool sandbox = false;
    /// dxFeed's event model gives Quote bidTime/askTime in milliseconds since the epoch;
    /// false (dxlink_time_unit=unknown) leaves market times at zero instead.
    bool timestamps_in_milliseconds = true;
  };
  explicit TastytradeProvider(Options options);
  ~TastytradeProvider() override { stop(); }
  [[nodiscard]] std::string_view name() const noexcept override { return "tastytrade"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;
  void start(const md::Subscription&, md::EventSink&) override;
  void stop() override;

 private:
  void run(md::Subscription subscription, md::EventSink* sink);
  Options options_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::mutex mutex_;
  std::condition_variable wake_;
};
}  // namespace openport::providers

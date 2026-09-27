#pragma once

#include <deque>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <tuple>
#include <vector>

#include "openport/md/provider.hpp"
#include "openport/net/websocket.hpp"

namespace openport::providers {
struct DxlinkSubscription {
  std::string type, symbol;
  friend bool operator==(const DxlinkSubscription&, const DxlinkSubscription&) = default;
  friend bool operator<(const DxlinkSubscription& a, const DxlinkSubscription& b) {
    return std::tie(a.type, a.symbol) < std::tie(b.type, b.symbol);
  }
};

/// Socket-independent DXLink FEED protocol; all timers use a supplied operational
/// clock. Feed timestamps remain vendor data and are never replaced by this clock.
class DxlinkProtocol {
 public:
  explicit DxlinkProtocol(net::WebSocket& transport) : transport_(transport) {}
  void begin(std::string token, std::set<DxlinkSubscription> subscriptions, md::Timestamp now);
  [[nodiscard]] std::vector<nlohmann::json> on_message(std::string_view message, md::Timestamp now);
  void tick(md::Timestamp now);
  void replace(std::set<DxlinkSubscription> subscriptions, md::Timestamp now);
  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] static nlohmann::json event_fields();
  [[nodiscard]] static std::chrono::seconds backoff(unsigned attempt);

 private:
  void send(nlohmann::json message, md::Timestamp now);
  void flush(md::Timestamp now);
  net::WebSocket& transport_;
  std::string token_;
  nlohmann::json fields_;
  std::set<DxlinkSubscription> desired_, active_;
  std::deque<std::pair<md::Timestamp, std::size_t>> changes_;
  md::Timestamp sent_ = 0, received_ = 0, action_ = 0;
  md::Timestamp heartbeat_ = 30 * md::kNanosPerSecond;
  bool auth_sent_ = false, authorized_ = false, opened_ = false, ready_ = false;
};
}  // namespace openport::providers

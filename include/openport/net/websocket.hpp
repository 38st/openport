#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace openport::net {
/// One TLS WebSocket, used from one worker. A receive timeout leaves the read
/// pending, so keepalives can be written without discarding a partial frame.
class WebSocket {
 public:
  virtual ~WebSocket() = default;
  virtual void connect(std::string_view url, const std::atomic<bool>* cancel) = 0;
  virtual void send(std::string_view message) = 0;
  virtual std::optional<std::string> receive(std::chrono::milliseconds timeout) = 0;
  virtual void close() noexcept = 0;
};
[[nodiscard]] std::unique_ptr<WebSocket> make_websocket();
}  // namespace openport::net

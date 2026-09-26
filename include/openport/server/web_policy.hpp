#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "openport/server/api.hpp"

namespace openport::server {

inline constexpr std::size_t kWebSocketMessageMax = 4 * 1024;
inline constexpr std::size_t kWebSocketSessionMax = 256;

struct WritePolicy {
  std::string address = "127.0.0.1";
  std::string token;
  std::vector<std::string> allowed_origins;
};
[[nodiscard]] std::string write_mode(const WritePolicy& policy);
/// One policy for all API writes, including routes that do not yet exist.
[[nodiscard]] std::optional<ApiResponse> check_api_write(const ApiRequest& request,
                                                        const WritePolicy& policy);

struct StaticFile {
  int status = 200;
  std::filesystem::path path;
  std::string reason;
};

/// Resolves only relative URL components, then checks canonical containment so
/// neither absolute paths nor symlinks can turn the server into a host-file reader.
[[nodiscard]] StaticFile resolve_static_file(const std::filesystem::path& root,
                                             std::string_view target);

/// Non-browser clients may omit Origin. Present origins must name the same
/// authority as Host; opaque origins and malformed authorities fail closed.
[[nodiscard]] bool websocket_origin_allowed(std::optional<std::string_view> origin,
                                            std::string_view host,
                                            const std::vector<std::string>& allowed_origins = {});

/// Canonical scheme and authority, without paths, credentials, or wildcards.
[[nodiscard]] std::optional<std::string> normalize_origin(std::string_view origin);

/// DNS rebinding points a name an attacker controls at this server, making a page
/// served under that name same-origin with it. The name arrives in Host, so a request
/// must address the server by an IP address, localhost (or a name under .localhost),
/// the host of an allowed origin, or an allowed host name such as a reverse proxy's
/// upstream. Names compare case-insensitively; ports are ignored.
[[nodiscard]] bool host_allowed(std::string_view host,
                                const std::vector<std::string>& allowed_origins = {},
                                const std::vector<std::string>& allowed_hosts = {});

/// Ticks are complete snapshots: preserve the active write buffer and only the
/// latest pending snapshot. All methods run on the session executor.
class TickQueue {
 public:
  void push(std::shared_ptr<const std::string> tick) {
    if (!writing_)
      writing_ = std::move(tick);
    else
      pending_ = std::move(tick);
  }
  void pop() { writing_ = std::move(pending_); }
  void clear() {
    writing_.reset();
    pending_.reset();
  }
  [[nodiscard]] bool empty() const { return !writing_; }
  [[nodiscard]] std::size_t size() const { return bool(writing_) + bool(pending_); }
  [[nodiscard]] const auto& front() const { return writing_; }

 private:
  std::shared_ptr<const std::string> writing_;
  std::shared_ptr<const std::string> pending_;
};

/// A slot covers the handshake as well as the open session and is returned on
/// every destruction path, including failed upgrades.
class WebSocketSlots {
 public:
  class Lease {
   public:
    ~Lease() { --owner_.active_; }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

   private:
    friend class WebSocketSlots;
    explicit Lease(WebSocketSlots& owner) : owner_(owner) {}
    WebSocketSlots& owner_;
  };

  [[nodiscard]] std::unique_ptr<Lease> acquire();

 private:
  std::atomic<std::size_t> active_{0};
};

}  // namespace openport::server

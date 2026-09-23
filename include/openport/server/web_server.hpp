#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "openport/server/api.hpp"

namespace openport::server {

using ApiHandler = std::function<ApiResponse(const ApiRequest&)>;

/// HTTP and WebSocket server for the web terminal.
///
/// - `/api/...` goes to the API handler.
/// - `/ws` upgrades to a WebSocket that receives every `broadcast()` message.
///   A client that falls behind gets the latest message rather than a backlog.
/// - Anything else is a file under `web_root`; unknown paths without an extension
///   fall back to index.html so the single-page app can route them.
class WebServer {
 public:
  WebServer(std::string address, unsigned short port, std::filesystem::path web_root,
            ApiHandler api, std::vector<std::string> allowed_origins = {});
  ~WebServer();
  WebServer(const WebServer&) = delete;
  WebServer& operator=(const WebServer&) = delete;

  /// Binds once per instance. A second start throws, even after stop or failure.
  /// Throws if the port is taken.
  void start(int threads = 1);
  void stop();

  /// Sends `message` to every connected WebSocket client. Thread-safe.
  void broadcast(std::string message);

  /// The bound port (useful when constructed with port 0).
  [[nodiscard]] unsigned short port() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace openport::server

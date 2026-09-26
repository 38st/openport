#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <type_traits>

#include "openport/server/api.hpp"

namespace openport::server {

using AsyncApiHandler = std::function<void(const ApiRequest&, ApiCompletion)>;
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
  /// `allowed_hosts` names the server may be addressed by besides IP addresses,
  /// localhost and the allowed origins' hosts (see host_allowed).
  WebServer(std::string address, unsigned short port, std::filesystem::path web_root,
            AsyncApiHandler api, std::vector<std::string> allowed_origins = {}, std::string write_token = {},
            std::vector<std::string> allowed_hosts = {});
  template <class Handler> requires std::is_invocable_r_v<ApiResponse, Handler, const ApiRequest&>
  WebServer(std::string address, unsigned short port, std::filesystem::path web_root,
            Handler api, std::vector<std::string> allowed_origins = {}, std::string write_token = {},
            std::vector<std::string> allowed_hosts = {})
      : WebServer(std::move(address), port, std::move(web_root),
                  AsyncApiHandler([api = std::move(api)](const ApiRequest& request, ApiCompletion complete) {
                    complete(api(request));
                  }), std::move(allowed_origins), std::move(write_token), std::move(allowed_hosts)) {}

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

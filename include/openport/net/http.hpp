#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openport::net {

struct Url {
  std::string host;
  std::string port;    ///< "443" unless the URL says otherwise
  std::string target;  ///< path and query
};

/// Parses "https://host[:port][/path][?query]". Only https is accepted.
[[nodiscard]] std::optional<Url> parse_https_url(std::string_view url);

struct HttpResponse {
  int status = 0;
  std::string body;            ///< decompressed if the server sent gzip
  std::size_t wire_bytes = 0;  ///< body bytes as received, before decompression
  std::chrono::microseconds elapsed{0};
};

using Headers = std::vector<std::pair<std::string, std::string>>;

/// A blocking HTTPS client. It keeps the TLS connection to the last host alive
/// between requests, asks for gzip and decompresses it, verifies certificates
/// against the system trust store, and enforces a timeout on every step.
///
/// Not thread-safe: give each thread its own client.
class HttpsClient {
 public:
  HttpsClient();
  ~HttpsClient();
  HttpsClient(const HttpsClient&) = delete;
  HttpsClient& operator=(const HttpsClient&) = delete;

  /// Throws std::runtime_error on network, TLS or timeout failures. HTTP error
  /// statuses are returned, not thrown.
  HttpResponse get(std::string_view url, const Headers& headers = {},
                   std::chrono::seconds timeout = std::chrono::seconds(30));

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Decompresses a complete gzip stream. Throws std::runtime_error if it is corrupt.
[[nodiscard]] std::string gunzip(std::string_view compressed);

}  // namespace openport::net

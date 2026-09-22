#include "openport/net/http.hpp"

#include <openssl/ssl.h>
#include <zlib.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace openport::net {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using Stream = beast::ssl_stream<beast::tcp_stream>;

constexpr std::size_t kMaxBody = 512u * 1024 * 1024;
constexpr std::string_view kUserAgent = "OpenPort/0.1 (+https://github.com/38st/openport)";

}  // namespace

std::optional<Url> parse_https_url(std::string_view url) {
  constexpr std::string_view scheme = "https://";
  if (!url.starts_with(scheme)) return std::nullopt;
  url.remove_prefix(scheme.size());

  const std::size_t path_start = url.find_first_of("/?");
  const std::string_view authority = url.substr(0, path_start);
  Url out;
  if (path_start == std::string_view::npos) {
    out.target = "/";
  } else {
    out.target = url.substr(path_start);
    if (out.target.front() == '?') out.target.insert(0, "/");
  }
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    out.host = authority;
    out.port = "443";
  } else {
    out.host = authority.substr(0, colon);
    out.port = authority.substr(colon + 1);
  }
  if (out.host.empty() || out.port.empty()) return std::nullopt;
  return out;
}

struct HttpsClient::Impl {
  asio::io_context io;
  ssl::context tls{ssl::context::tls_client};
  std::unique_ptr<Stream> stream;
  std::string host;
  std::string port;
  beast::flat_buffer buffer;

  Impl() {
    tls.set_verify_mode(ssl::verify_peer);
    tls.set_default_verify_paths();
    // OpenSSL's compiled-in trust store is not always populated; add the platform bundle.
    for (const char* bundle : {"/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt"}) {
      if (std::filesystem::exists(bundle)) {
        boost::system::error_code ignored;
        tls.load_verify_file(bundle, ignored);
        break;
      }
    }
  }

  // Beast only enforces tcp_stream timeouts on asynchronous operations, so every
  // step is started asynchronously and the io_context is run until it finishes.
  template <typename Start>
  void run(Start&& start) {
    boost::system::error_code result = asio::error::would_block;
    std::forward<Start>(start)([&result](boost::system::error_code ec) { result = ec; });
    io.restart();
    io.run();
    if (result) throw boost::system::system_error(result);
  }

  void close() noexcept {
    if (!stream) return;
    boost::system::error_code ignored;
    beast::get_lowest_layer(*stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
    beast::get_lowest_layer(*stream).socket().close(ignored);
    stream.reset();
    buffer.clear();
  }

  void connect(const Url& url, std::chrono::seconds timeout) {
    close();
    auto fresh = std::make_unique<Stream>(io, tls);
    if (SSL_set_tlsext_host_name(fresh->native_handle(), url.host.c_str()) != 1) {
      throw std::runtime_error("TLS: cannot set SNI host name");
    }
    fresh->set_verify_callback(ssl::host_name_verification(url.host));

    tcp::resolver resolver(io);
    const auto endpoints = resolver.resolve(url.host, url.port);
    auto& tcp_stream = beast::get_lowest_layer(*fresh);
    tcp_stream.expires_after(timeout);
    run([&](auto done) {
      tcp_stream.async_connect(endpoints, [done](boost::system::error_code ec,
                                                 const tcp::endpoint&) { done(ec); });
    });
    tcp_stream.expires_after(timeout);
    run([&](auto done) { fresh->async_handshake(ssl::stream_base::client, done); });

    stream = std::move(fresh);
    host = url.host;
    port = url.port;
  }

  HttpResponse request(const Url& url, const Headers& headers, std::chrono::seconds timeout) {
    http::request<http::empty_body> req{http::verb::get, url.target, 11};
    req.set(http::field::host, url.host);
    req.set(http::field::user_agent, kUserAgent);
    req.set(http::field::accept_encoding, "gzip");
    for (const auto& [name, value] : headers) req.set(name, value);

    const auto started = std::chrono::steady_clock::now();
    auto& tcp_stream = beast::get_lowest_layer(*stream);
    tcp_stream.expires_after(timeout);
    run([&](auto done) {
      http::async_write(*stream, req,
                        [done](boost::system::error_code ec, std::size_t) { done(ec); });
    });

    http::response_parser<http::string_body> parser;
    parser.body_limit(kMaxBody);
    tcp_stream.expires_after(timeout);
    run([&](auto done) {
      http::async_read(*stream, buffer, parser,
                       [done](boost::system::error_code ec, std::size_t) { done(ec); });
    });
    tcp_stream.expires_never();

    auto response = parser.release();
    HttpResponse out;
    out.status = static_cast<int>(response.result_int());
    out.wire_bytes = response.body().size();
    const bool gzipped = beast::iequals(response[http::field::content_encoding], "gzip");
    out.body = gzipped ? gunzip(response.body()) : std::move(response.body());
    out.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    if (!response.keep_alive()) close();
    return out;
  }
};

HttpsClient::HttpsClient() : impl_(std::make_unique<Impl>()) {}

HttpsClient::~HttpsClient() { impl_->close(); }

HttpResponse HttpsClient::get(std::string_view url, const Headers& headers,
                              std::chrono::seconds timeout) {
  const std::optional<Url> parsed = parse_https_url(url);
  if (!parsed) throw std::runtime_error("not an https URL: " + std::string(url));

  const bool reusing = impl_->stream && impl_->host == parsed->host && impl_->port == parsed->port;
  try {
    if (!reusing) impl_->connect(*parsed, timeout);
    return impl_->request(*parsed, headers, timeout);
  } catch (const std::exception&) {
    impl_->close();
    if (!reusing) throw;
  }
  // A kept-alive connection can be closed by the server between requests; retry once fresh.
  impl_->connect(*parsed, timeout);
  return impl_->request(*parsed, headers, timeout);
}

std::string gunzip(std::string_view compressed) {
  z_stream zs{};
  if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) throw std::runtime_error("gzip: init failed");
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
  zs.avail_in = static_cast<uInt>(compressed.size());

  std::string out;
  out.resize(compressed.size() * 8 + 1024);
  int status = Z_OK;
  while (status == Z_OK) {
    if (zs.total_out >= out.size()) out.resize(out.size() * 2);
    zs.next_out = reinterpret_cast<Bytef*>(out.data() + zs.total_out);
    const std::size_t space = out.size() - zs.total_out;
    zs.avail_out = static_cast<uInt>(std::min<std::size_t>(space, std::numeric_limits<uInt>::max()));
    status = inflate(&zs, Z_NO_FLUSH);
  }
  const std::size_t produced = zs.total_out;
  inflateEnd(&zs);
  if (status != Z_STREAM_END) throw std::runtime_error("gzip: corrupt or truncated stream");
  out.resize(produced);
  return out;
}

}  // namespace openport::net

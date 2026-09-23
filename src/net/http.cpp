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
using TlsStream = beast::ssl_stream<beast::tcp_stream>;

constexpr std::size_t kMaxBody = 512u * 1024 * 1024;
constexpr std::string_view kUserAgent = "OpenPort/0.1 (+https://github.com/38st/openport)";

}  // namespace

std::optional<Url> parse_url(std::string_view url) {
  Url out;
  if (url.starts_with("https://")) {
    url.remove_prefix(8);
  } else if (url.starts_with("http://")) {
    url.remove_prefix(7);
    out.tls = false;
  } else {
    return std::nullopt;
  }

  const std::size_t path_start = url.find_first_of("/?");
  const std::string_view authority = url.substr(0, path_start);
  if (path_start == std::string_view::npos) {
    out.target = "/";
  } else {
    out.target = url.substr(path_start);
    if (out.target.front() == '?') out.target.insert(0, "/");
  }
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    out.host = authority;
    out.port = out.tls ? "443" : "80";
  } else {
    out.host = authority.substr(0, colon);
    out.port = authority.substr(colon + 1);
  }
  if (out.host.empty() || out.port.empty()) return std::nullopt;
  return out;
}

struct HttpClient::Impl {
  asio::io_context io;
  ssl::context tls_context{ssl::context::tls_client};
  // Exactly one of these is open at a time.
  std::unique_ptr<TlsStream> tls;
  std::unique_ptr<beast::tcp_stream> plain;
  Url connected;
  beast::flat_buffer buffer;

  Impl() {
    tls_context.set_verify_mode(ssl::verify_peer);
    tls_context.set_default_verify_paths();
    // OpenSSL's compiled-in trust store is not always populated; add the platform bundle.
    for (const char* bundle : {"/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt"}) {
      if (std::filesystem::exists(bundle)) {
        boost::system::error_code ignored;
        tls_context.load_verify_file(bundle, ignored);
        break;
      }
    }
  }

  [[nodiscard]] bool open() const noexcept { return tls || plain; }

  [[nodiscard]] bool connected_to(const Url& url) const noexcept {
    return open() && connected.tls == url.tls && connected.host == url.host &&
           connected.port == url.port;
  }

  [[nodiscard]] beast::tcp_stream& tcp_layer() {
    return tls ? beast::get_lowest_layer(*tls) : *plain;
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
    if (!open()) return;
    boost::system::error_code ignored;
    tcp_layer().socket().shutdown(tcp::socket::shutdown_both, ignored);
    tcp_layer().socket().close(ignored);
    tls.reset();
    plain.reset();
    buffer.clear();
  }

  void connect(const Url& url, std::chrono::seconds timeout) {
    close();
    tcp::resolver resolver(io);
    const auto endpoints = resolver.resolve(url.host, url.port);

    if (url.tls) {
      auto stream = std::make_unique<TlsStream>(io, tls_context);
      // SSL_set_tlsext_host_name is a macro with a C-style cast; call what it expands to.
      if (SSL_ctrl(stream->native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
                   const_cast<char*>(url.host.c_str())) != 1) {
        throw std::runtime_error("TLS: cannot set SNI host name");
      }
      stream->set_verify_callback(ssl::host_name_verification(url.host));
      auto& layer = beast::get_lowest_layer(*stream);
      layer.expires_after(timeout);
      run([&](auto done) {
        layer.async_connect(endpoints,
                            [done](boost::system::error_code ec, const tcp::endpoint&) { done(ec); });
      });
      layer.expires_after(timeout);
      run([&](auto done) { stream->async_handshake(ssl::stream_base::client, done); });
      tls = std::move(stream);
    } else {
      auto stream = std::make_unique<beast::tcp_stream>(io);
      stream->expires_after(timeout);
      run([&](auto done) {
        stream->async_connect(endpoints,
                              [done](boost::system::error_code ec, const tcp::endpoint&) { done(ec); });
      });
      plain = std::move(stream);
    }
    connected = url;
  }

  template <typename Stream>
  HttpResponse exchange(Stream& stream, const Url& url, const Headers& headers,
                        std::chrono::seconds timeout) {
    http::request<http::empty_body> req{http::verb::get, url.target, 11};
    req.set(http::field::host, url.host);
    req.set(http::field::user_agent, kUserAgent);
    req.set(http::field::accept_encoding, "gzip");
    for (const auto& [name, value] : headers) req.set(name, value);

    const auto started = std::chrono::steady_clock::now();
    tcp_layer().expires_after(timeout);
    run([&](auto done) {
      http::async_write(stream, req, [done](boost::system::error_code ec, std::size_t) { done(ec); });
    });

    http::response_parser<http::string_body> parser;
    parser.body_limit(kMaxBody);
    tcp_layer().expires_after(timeout);
    run([&](auto done) {
      http::async_read(stream, buffer, parser,
                       [done](boost::system::error_code ec, std::size_t) { done(ec); });
    });
    tcp_layer().expires_never();

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

  HttpResponse request(const Url& url, const Headers& headers, std::chrono::seconds timeout) {
    return tls ? exchange(*tls, url, headers, timeout) : exchange(*plain, url, headers, timeout);
  }
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}

HttpClient::~HttpClient() { impl_->close(); }

HttpResponse HttpClient::get(std::string_view url, const Headers& headers,
                             std::chrono::seconds timeout) {
  const std::optional<Url> parsed = parse_url(url);
  if (!parsed) throw std::runtime_error("not an http(s) URL: " + std::string(url));

  const bool reusing = impl_->connected_to(*parsed);
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
  if (compressed.size() > std::numeric_limits<uInt>::max()) {
    throw std::runtime_error("gzip: compressed input too large");
  }
  z_stream zs{};
  if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) throw std::runtime_error("gzip: init failed");
  struct EndInflate {
    z_stream& stream;
    ~EndInflate() { inflateEnd(&stream); }
  } end{zs};
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
  zs.avail_in = static_cast<uInt>(compressed.size());

  std::string out;
  char chunk[64 * 1024];
  int status = Z_OK;
  while (status == Z_OK) {
    zs.next_out = reinterpret_cast<Bytef*>(chunk);
    zs.avail_out = sizeof chunk;
    status = inflate(&zs, Z_NO_FLUSH);
    const std::size_t produced = sizeof chunk - zs.avail_out;
    if (produced > kMaxDecompressedBytes - out.size()) {
      throw std::runtime_error("gzip: decompressed output exceeds 256 MiB");
    }
    out.append(chunk, produced);
  }
  if (status != Z_STREAM_END) throw std::runtime_error("gzip: corrupt or truncated stream");
  return out;
}

}  // namespace openport::net

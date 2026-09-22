#include "openport/server/web_server.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace openport::server {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

std::string_view mime_type(std::string_view path) {
  const std::size_t dot = path.rfind('.');
  const std::string_view ext = dot == std::string_view::npos ? "" : path.substr(dot);
  if (ext == ".html") return "text/html; charset=utf-8";
  if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
  if (ext == ".css") return "text/css; charset=utf-8";
  if (ext == ".json") return "application/json";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".png") return "image/png";
  if (ext == ".ico") return "image/x-icon";
  if (ext == ".woff2") return "font/woff2";
  if (ext == ".woff") return "font/woff";
  if (ext == ".txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

class WsSession;

/// Tracks live WebSocket sessions and fans messages out to them.
class Hub {
 public:
  void add(std::weak_ptr<WsSession> session) {
    const std::lock_guard lock(mutex_);
    sessions_.push_back(std::move(session));
  }

  void broadcast(const std::shared_ptr<const std::string>& message);

 private:
  std::mutex mutex_;
  std::vector<std::weak_ptr<WsSession>> sessions_;
};

class WsSession : public std::enable_shared_from_this<WsSession> {
 public:
  WsSession(tcp::socket&& socket, Hub& hub) : ws_(std::move(socket)), hub_(hub) {}

  void accept(http::request<http::string_body> request) {
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.async_accept(request, beast::bind_front_handler(&WsSession::on_accept, shared_from_this()));
  }

  void send(std::shared_ptr<const std::string> message) {
    asio::post(ws_.get_executor(), [self = shared_from_this(), message = std::move(message)] {
      if (!self->open_) return;
      // Keep at most one message waiting behind the one being written: a slow client
      // gets the newest state, not an ever-growing queue.
      if (self->queue_.size() >= 2) {
        self->queue_.back() = message;
        return;
      }
      self->queue_.push_back(message);
      if (self->queue_.size() == 1) self->write_next();
    });
  }

 private:
  void on_accept(beast::error_code ec) {
    if (ec) return;
    open_ = true;
    hub_.add(weak_from_this());
    read();
  }

  void read() {
    ws_.async_read(buffer_, beast::bind_front_handler(&WsSession::on_read, shared_from_this()));
  }

  void on_read(beast::error_code ec, std::size_t) {
    if (ec) {
      open_ = false;
      return;
    }
    buffer_.consume(buffer_.size());  // clients have nothing to say yet
    read();
  }

  void write_next() {
    ws_.text(true);
    ws_.async_write(asio::buffer(*queue_.front()),
                    beast::bind_front_handler(&WsSession::on_write, shared_from_this()));
  }

  void on_write(beast::error_code ec, std::size_t) {
    if (ec) {
      open_ = false;
      queue_.clear();
      return;
    }
    queue_.pop_front();
    if (!queue_.empty()) write_next();
  }

  websocket::stream<beast::tcp_stream> ws_;
  beast::flat_buffer buffer_;
  Hub& hub_;
  std::deque<std::shared_ptr<const std::string>> queue_;
  bool open_ = false;
};

void Hub::broadcast(const std::shared_ptr<const std::string>& message) {
  const std::lock_guard lock(mutex_);
  std::erase_if(sessions_, [&](const std::weak_ptr<WsSession>& weak) {
    const auto session = weak.lock();
    if (!session) return true;
    session->send(message);
    return false;
  });
}

struct Shared {
  std::filesystem::path web_root;
  ApiHandler api;
  Hub hub;
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket&& socket, Shared& shared) : stream_(std::move(socket)), shared_(shared) {}

  void run() {
    asio::dispatch(stream_.get_executor(),
                   beast::bind_front_handler(&HttpSession::read, shared_from_this()));
  }

 private:
  void read() {
    parser_.emplace();
    parser_->body_limit(64 * 1024);
    stream_.expires_after(std::chrono::seconds(60));
    http::async_read(stream_, buffer_, *parser_,
                     beast::bind_front_handler(&HttpSession::on_read, shared_from_this()));
  }

  void on_read(beast::error_code ec, std::size_t) {
    if (ec == http::error::end_of_stream) return close();
    if (ec) return;
    http::request<http::string_body> request = parser_->release();

    if (websocket::is_upgrade(request)) {
      if (request.target() == "/ws") {
        stream_.expires_never();
        std::make_shared<WsSession>(stream_.release_socket(), shared_.hub)->accept(std::move(request));
      }
      return;
    }
    respond(handle(std::move(request)));
  }

  http::message_generator handle(http::request<http::string_body>&& request) {
    const std::string target(request.target());
    if (target.starts_with("/api/")) {
      ApiResponse api;
      try {
        api = shared_.api(ApiRequest{std::string(request.method_string()), target});
      } catch (const std::exception&) {
        api = {500, std::string(R"({"error":"internal error"})")};
      }
      http::response<http::string_body> response{static_cast<http::status>(api.status),
                                                 request.version()};
      response.set(http::field::content_type, "application/json");
      response.set(http::field::cache_control, "no-store");
      response.set(http::field::access_control_allow_origin, "*");
      response.keep_alive(request.keep_alive());
      response.body() = std::move(api.body);
      response.prepare_payload();
      return response;
    }
    return serve_file(request, target);
  }

  http::message_generator serve_file(const http::request<http::string_body>& request,
                                     std::string target) {
    auto text = [&](http::status status, std::string_view body) {
      http::response<http::string_body> response{status, request.version()};
      response.set(http::field::content_type, "text/plain; charset=utf-8");
      response.keep_alive(request.keep_alive());
      response.body() = std::string(body);
      response.prepare_payload();
      return http::message_generator(std::move(response));
    };
    if (request.method() != http::verb::get && request.method() != http::verb::head) {
      return text(http::status::method_not_allowed, "method not allowed\n");
    }
    if (const std::size_t q = target.find('?'); q != std::string::npos) target.resize(q);
    if (target.empty() || target.front() != '/' || target.find("..") != std::string::npos) {
      return text(http::status::bad_request, "bad path\n");
    }
    if (target == "/") target = "/index.html";

    std::filesystem::path file = shared_.web_root / target.substr(1);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
      // Client-side routes ("/chain/SPX") are served the app shell.
      if (std::filesystem::path(target).has_extension()) {
        return text(http::status::not_found, "not found\n");
      }
      file = shared_.web_root / "index.html";
      if (!std::filesystem::is_regular_file(file, ec)) {
        return text(http::status::not_found,
                    "web terminal not built: run `npm run build` in web/, or pass --web-root\n");
      }
    }

    beast::error_code open_error;
    http::file_body::value_type body;
    body.open(file.string().c_str(), beast::file_mode::scan, open_error);
    if (open_error) return text(http::status::internal_server_error, "cannot read file\n");
    const auto size = body.size();

    const bool fingerprinted = target.starts_with("/assets/");
    if (request.method() == http::verb::head) {
      http::response<http::empty_body> response{http::status::ok, request.version()};
      response.set(http::field::content_type, mime_type(file.string()));
      response.content_length(size);
      response.keep_alive(request.keep_alive());
      return response;
    }
    http::response<http::file_body> response{std::piecewise_construct,
                                             std::make_tuple(std::move(body)),
                                             std::make_tuple(http::status::ok, request.version())};
    response.set(http::field::content_type, mime_type(file.string()));
    // Built assets carry a content hash in their name and never change; the shell must.
    response.set(http::field::cache_control,
                 fingerprinted ? "public, max-age=31536000, immutable" : "no-cache");
    response.content_length(size);
    response.keep_alive(request.keep_alive());
    return response;
  }

  void respond(http::message_generator&& message) {
    const bool keep_alive = message.keep_alive();
    beast::async_write(stream_, std::move(message),
                       [self = shared_from_this(), keep_alive](beast::error_code ec, std::size_t) {
                         if (ec) return;
                         if (!keep_alive) return self->close();
                         self->read();
                       });
  }

  void close() {
    beast::error_code ignored;
    stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
  }

  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  std::optional<http::request_parser<http::string_body>> parser_;
  Shared& shared_;
};

class Listener : public std::enable_shared_from_this<Listener> {
 public:
  Listener(asio::io_context& io, const tcp::endpoint& endpoint, Shared& shared)
      : io_(io), acceptor_(asio::make_strand(io)), shared_(shared) {
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(asio::socket_base::max_listen_connections);
    port_ = acceptor_.local_endpoint().port();
  }

  void accept() {
    acceptor_.async_accept(asio::make_strand(io_),
                           beast::bind_front_handler(&Listener::on_accept, shared_from_this()));
  }

  [[nodiscard]] unsigned short port() const noexcept { return port_; }

  void close() {
    asio::post(acceptor_.get_executor(), [self = shared_from_this()] {
      beast::error_code ignored;
      self->acceptor_.close(ignored);
    });
  }

 private:
  void on_accept(beast::error_code ec, tcp::socket socket) {
    if (!acceptor_.is_open()) return;
    if (!ec) std::make_shared<HttpSession>(std::move(socket), shared_)->run();
    accept();
  }

  asio::io_context& io_;
  tcp::acceptor acceptor_;
  Shared& shared_;
  unsigned short port_ = 0;
};

}  // namespace

struct WebServer::Impl {
  // Declared in this order so the io_context (and every pending session in it) is
  // destroyed before the state those sessions refer to.
  Shared shared;
  std::string address;
  unsigned short requested_port;
  asio::io_context io;
  std::shared_ptr<Listener> listener;
  std::vector<std::thread> threads;
};

WebServer::WebServer(std::string address, unsigned short port, std::filesystem::path web_root,
                     ApiHandler api)
    : impl_(std::make_unique<Impl>()) {
  impl_->shared.web_root = std::move(web_root);
  impl_->shared.api = std::move(api);
  impl_->address = std::move(address);
  impl_->requested_port = port;
}

WebServer::~WebServer() { stop(); }

void WebServer::start(int threads) {
  const tcp::endpoint endpoint{asio::ip::make_address(impl_->address), impl_->requested_port};
  impl_->listener = std::make_shared<Listener>(impl_->io, endpoint, impl_->shared);
  impl_->listener->accept();
  for (int i = 0; i < std::max(1, threads); ++i) {
    impl_->threads.emplace_back([this] { impl_->io.run(); });
  }
}

void WebServer::stop() {
  if (impl_->threads.empty()) return;
  if (impl_->listener) impl_->listener->close();
  impl_->io.stop();
  for (std::thread& thread : impl_->threads) thread.join();
  impl_->threads.clear();
}

void WebServer::broadcast(std::string message) {
  impl_->shared.hub.broadcast(std::make_shared<const std::string>(std::move(message)));
}

unsigned short WebServer::port() const noexcept {
  return impl_->listener ? impl_->listener->port() : impl_->requested_port;
}

}  // namespace openport::server

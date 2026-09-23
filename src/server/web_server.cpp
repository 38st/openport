#include "openport/server/web_server.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <charconv>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "openport/server/web_policy.hpp"

namespace openport::server {

StaticFile resolve_static_file(const std::filesystem::path& root, std::string_view target) {
  target = target.substr(0, target.find('?'));
  if (target.empty() || target.front() != '/') return {400, {}, "path must start with one slash"};
  target.remove_prefix(1);
  std::filesystem::path relative;
  while (!target.empty()) {
    const auto slash = target.find('/');
    const auto encoded = target.substr(0, slash);
    if (encoded.empty()) return {400, {}, "absolute or empty path component"};
    std::string component;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
      char c = encoded[i];
      if (c == '%') {
        if (i + 2 >= encoded.size()) return {400, {}, "malformed path escape"};
        unsigned int byte = 0;
        const auto [end, ec] =
            std::from_chars(encoded.data() + i + 1, encoded.data() + i + 3, byte, 16);
        if (ec != std::errc{} || end != encoded.data() + i + 3) {
          return {400, {}, "malformed path escape"};
        }
        c = static_cast<char>(byte);
        i += 2;
      }
      if (c == '/' || c == '\\' || c == '\0') return {400, {}, "forbidden path separator or NUL"};
      component += c;
    }
    const std::filesystem::path part(component);
    if (component == ".." || part.has_root_path()) return {400, {}, "forbidden path component"};
    relative /= part;
    if (slash == std::string_view::npos) break;
    target.remove_prefix(slash + 1);
  }

  std::error_code ec;
  const auto canonical_root = std::filesystem::canonical(root, ec);
  if (ec || !std::filesystem::is_directory(canonical_root, ec)) {
    return {404, {}, "web terminal not built: run `npm run build` in web/, or pass --web-root"};
  }
  auto resolve = [&](const std::filesystem::path& path) -> StaticFile {
    const auto file = std::filesystem::weakly_canonical(canonical_root / path, ec);
    if (ec) return {403, {}, "cannot resolve path safely"};
    auto component = file.begin();
    for (const auto& root_component : canonical_root) {
      if (component == file.end() || *component++ != root_component) {
        return {403, {}, "path escapes web root"};
      }
    }
    if (!std::filesystem::is_regular_file(file, ec)) return {404, {}, "not a regular file"};
    return {200, file, {}};
  };
  if (relative.empty()) relative = "index.html";
  StaticFile file = resolve(relative);
  if (file.status != 404 || relative.has_extension()) return file;
  // Only missing client-side routes get the shell; directories and special files do not.
  const bool exists = std::filesystem::exists(canonical_root / relative, ec);
  if (ec || exists) return file;
  return resolve("index.html");
}

std::optional<std::string> normalize_origin(std::string_view origin) {
  const auto scheme = origin.find("://");
  if (scheme == std::string_view::npos) return std::nullopt;
  const auto protocol = origin.substr(0, scheme);
  if (protocol != "http" && protocol != "https") return std::nullopt;
  const auto authority = origin.substr(scheme + 3);
  auto normalize = [protocol](std::string_view value) -> std::optional<std::string> {
    if (value.empty() || value.find_first_of("/@\\?#% \t\r\n") != std::string_view::npos ||
        value.find('\0') != std::string_view::npos)
      return std::nullopt;
    const auto colon = value.rfind(':');
    const bool has_port =
        colon != std::string_view::npos && (value.front() != '[' || colon > value.find(']'));
    unsigned int port = protocol == "https" ? 443 : 80;
    auto name = value;
    if (has_port) {
      name = value.substr(0, colon);
      const auto text = value.substr(colon + 1);
      const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), port);
      if (ec != std::errc{} || end != text.data() + text.size() || port == 0 || port > 65535) {
        return std::nullopt;
      }
    }
    if (name.empty()) return std::nullopt;
    if (name.front() == '[') {
      boost::system::error_code ec;
      if (name.back() != ']') return std::nullopt;
      boost::asio::ip::make_address_v6(std::string(name.substr(1, name.size() - 2)), ec);
      if (ec) return std::nullopt;
    } else {
      for (const unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.'))
          return std::nullopt;
      }
    }
    std::string normalized(name);
    for (char& c : normalized)
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return normalized + ":" + std::to_string(port);
  };
  const auto source = normalize(authority);
  if (!source) return std::nullopt;
  return std::string(protocol) + "://" + *source;
}

bool websocket_origin_allowed(std::optional<std::string_view> origin, std::string_view host,
                              const std::vector<std::string>& allowed_origins) {
  if (!origin) return true;
  const auto source = normalize_origin(*origin);
  if (!source) return false;
  const auto scheme = source->substr(0, source->find("://") + 3);
  const auto destination = normalize_origin(scheme + std::string(host));
  if (source == destination) return true;
  for (const auto& allowed : allowed_origins) {
    if (source == normalize_origin(allowed)) return true;
  }
  return false;
}

std::unique_ptr<WebSocketSlots::Lease> WebSocketSlots::acquire() {
  auto count = active_.load();
  do {
    if (count >= kWebSocketSessionMax) return nullptr;
  } while (!active_.compare_exchange_weak(count, count + 1));
  try {
    return std::unique_ptr<Lease>(new Lease(*this));
  } catch (...) {
    --active_;
    throw;
  }
}

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

class Session {
 public:
  virtual ~Session() = default;
  virtual std::future<void> stop() = 0;
};

class Sessions {
 public:
  bool add(const std::shared_ptr<Session>& session) {
    const std::lock_guard lock(mutex_);
    if (stopping_) return false;
    std::erase_if(sessions_, [](const auto& weak) { return weak.expired(); });
    sessions_.push_back(session);
    return true;
  }

  void stop(asio::io_context* local_io = nullptr) {
    std::vector<std::shared_ptr<Session>> active;
    {
      const std::lock_guard lock(mutex_);
      stopping_ = true;
      for (auto& weak : sessions_)
        if (auto session = weak.lock()) active.push_back(session);
    }
    std::vector<std::future<void>> closed;
    for (auto& session : active) closed.push_back(session->stop());
    for (auto& done : closed) {
      if (local_io) {
        while (done.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
          local_io->poll_one();
      }
      done.get();
    }
  }

 private:
  std::mutex mutex_;
  std::vector<std::weak_ptr<Session>> sessions_;
  bool stopping_ = false;
};

class WsSession;

/// Tracks live WebSocket sessions and fans messages out to them.
class Hub {
 public:
  void add(std::weak_ptr<WsSession> session) {
    const std::lock_guard lock(mutex_);
    std::erase_if(sessions_, [](const auto& weak) { return weak.expired(); });
    sessions_.push_back(std::move(session));
  }

  void broadcast(const std::shared_ptr<const std::string>& message);

 private:
  std::mutex mutex_;
  std::vector<std::weak_ptr<WsSession>> sessions_;
};

class WsSession : public Session, public std::enable_shared_from_this<WsSession> {
 public:
  WsSession(tcp::socket&& socket, Hub& hub, std::unique_ptr<WebSocketSlots::Lease> slot)
      : ws_(std::move(socket)), hub_(hub), slot_(std::move(slot)) {
    ws_.read_message_max(kWebSocketMessageMax);
  }

  void accept(http::request<http::string_body> request) {
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.async_accept(request, beast::bind_front_handler(&WsSession::on_accept, shared_from_this()));
  }

  std::future<void> stop() override {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    asio::post(ws_.get_executor(), [self = shared_from_this(), done] {
      self->open_ = false;
      self->stopped_ = true;
      beast::error_code ignored;
      auto& socket = beast::get_lowest_layer(self->ws_).socket();
      socket.cancel(ignored);
      socket.shutdown(tcp::socket::shutdown_both, ignored);
      socket.close(ignored);
      // The active write owns its buffer until its cancellation handler runs.
      done->set_value();
    });
    return future;
  }

  void send(std::shared_ptr<const std::string> message) {
    asio::post(ws_.get_executor(), [self = shared_from_this(), message = std::move(message)] {
      if (!self->open_) return;
      const bool idle = self->queue_.empty();
      self->queue_.push(std::move(message));
      if (idle) self->write_next();
    });
  }

 private:
  void on_accept(beast::error_code ec) {
    if (ec || stopped_) return;
    open_ = true;
    hub_.add(weak_from_this());
    read();
  }

  void read() {
    ws_.async_read(buffer_, beast::bind_front_handler(&WsSession::on_read, shared_from_this()));
  }

  void on_read(beast::error_code ec, std::size_t) {
    if (ec || stopped_) {
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
    if (ec || stopped_) {
      open_ = false;
      queue_.clear();
      return;
    }
    queue_.pop();
    if (!queue_.empty()) write_next();
  }

  websocket::stream<beast::tcp_stream> ws_;
  beast::flat_buffer buffer_;
  Hub& hub_;
  std::unique_ptr<WebSocketSlots::Lease> slot_;
  TickQueue queue_;
  bool stopped_ = false;
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
  WebSocketSlots slots;
  Sessions sessions;
  std::vector<std::string> allowed_origins;
};

class HttpSession : public Session, public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket&& socket, Shared& shared) : stream_(std::move(socket)), shared_(shared) {}

  void run() {
    if (!shared_.sessions.add(shared_from_this())) return;
    asio::dispatch(stream_.get_executor(),
                   beast::bind_front_handler(&HttpSession::read, shared_from_this()));
  }

  std::future<void> stop() override {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    asio::post(stream_.get_executor(), [self = shared_from_this(), done] {
      self->stopped_ = true;
      self->close();
      done->set_value();
    });
    return future;
  }

 private:
  void read() {
    if (stopped_) return;
    parser_.emplace();
    parser_->body_limit(64 * 1024);
    stream_.expires_after(std::chrono::seconds(60));
    http::async_read(stream_, buffer_, *parser_,
                     beast::bind_front_handler(&HttpSession::on_read, shared_from_this()));
  }

  void on_read(beast::error_code ec, std::size_t) {
    if (stopped_ || ec == http::error::end_of_stream) return close();
    if (ec) return;
    http::request<http::string_body> request = parser_->release();

    if (websocket::is_upgrade(request)) {
      if (request.target() == "/ws") {
        const auto origin = request.find(http::field::origin);
        const auto origin_value = origin == request.end()
                                      ? std::nullopt
                                      : std::optional<std::string_view>(origin->value());
        if (request.count(http::field::origin) > 1 || request.count(http::field::host) != 1 ||
            !websocket_origin_allowed(origin_value, request[http::field::host],
                                      shared_.allowed_origins)) {
          return reject_upgrade(request, http::status::forbidden,
                                "WebSocket Origin does not match Host");
        }
        auto slot = shared_.slots.acquire();
        if (!slot)
          return reject_upgrade(request, http::status::service_unavailable,
                                "WebSocket session limit reached");
        stream_.expires_never();
        auto session =
            std::make_shared<WsSession>(stream_.release_socket(), shared_.hub, std::move(slot));
        // During shutdown an upgrade must not escape the registry's close barrier.
        if (shared_.sessions.add(session)) session->accept(std::move(request));
      }
      return;
    }
    respond(handle(std::move(request)));
  }

  void reject_upgrade(const http::request<http::string_body>& request, http::status status,
                      std::string reason) {
    http::response<http::string_body> response{status, request.version()};
    response.set(http::field::content_type, "text/plain; charset=utf-8");
    response.keep_alive(false);
    response.body() = std::move(reason);
    response.prepare_payload();
    respond(http::message_generator(std::move(response)));
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
    const StaticFile resolved = resolve_static_file(shared_.web_root, target);
    if (resolved.status != 200)
      return text(static_cast<http::status>(resolved.status), resolved.reason);
    const auto& file = resolved.path;

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
    stream_.socket().cancel(ignored);
    stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
    stream_.socket().close(ignored);
  }

  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  std::optional<http::request_parser<http::string_body>> parser_;
  Shared& shared_;
  bool stopped_ = false;
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

  std::future<void> close() {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    asio::post(acceptor_.get_executor(), [self = shared_from_this(), done] {
      beast::error_code ignored;
      self->acceptor_.close(ignored);
      done->set_value();
    });
    return future;
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
  bool started = false;
  std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
};

WebServer::WebServer(std::string address, unsigned short port, std::filesystem::path web_root,
                     ApiHandler api, std::vector<std::string> allowed_origins)
    : impl_(std::make_unique<Impl>()) {
  impl_->shared.web_root = std::move(web_root);
  impl_->shared.api = std::move(api);
  for (const auto& origin : allowed_origins) {
    if (!normalize_origin(origin)) throw std::invalid_argument("invalid allowed origin: " + origin);
  }
  impl_->shared.allowed_origins = std::move(allowed_origins);
  impl_->address = std::move(address);
  impl_->requested_port = port;
}

WebServer::~WebServer() { stop(); }

void WebServer::start(int threads) {
  if (impl_->started)
    throw std::logic_error("WebServer::start may be called only once; construct a new WebServer");
  impl_->started = true;
  impl_->work.emplace(impl_->io.get_executor());
  const tcp::endpoint endpoint{asio::ip::make_address(impl_->address), impl_->requested_port};
  impl_->listener = std::make_shared<Listener>(impl_->io, endpoint, impl_->shared);
  impl_->listener->accept();
  try {
    for (int i = 0; i < std::max(1, threads); ++i) {
      impl_->threads.emplace_back([this] { impl_->io.run(); });
    }
  } catch (...) {
    stop();
    throw;
  }
}

void WebServer::stop() {
  if (!impl_->listener) return;
  auto closed = impl_->listener->close();
  // A failed first thread launch still needs to execute the listener close.
  if (impl_->threads.empty()) {
    while (closed.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      impl_->io.poll_one();
  }
  closed.get();
  impl_->shared.sessions.stop(impl_->threads.empty() ? &impl_->io : nullptr);
  // Let cancellation completions release sessions before the I/O threads exit.
  impl_->work.reset();
  if (impl_->threads.empty()) impl_->io.run();
  for (std::thread& thread : impl_->threads) thread.join();
  impl_->threads.clear();
  impl_->io.stop();
  impl_->listener.reset();
}

void WebServer::broadcast(std::string message) {
  impl_->shared.hub.broadcast(std::make_shared<const std::string>(std::move(message)));
}

unsigned short WebServer::port() const noexcept {
  return impl_->listener ? impl_->listener->port() : impl_->requested_port;
}

}  // namespace openport::server

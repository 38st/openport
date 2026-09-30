#include "openport/server/web_policy.hpp"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <fstream>
#include <condition_variable>
#include <future>
#include <thread>
#include <vector>

#include "openport/server/sandboxes.hpp"
#include "openport/server/web_server.hpp"

namespace {

using namespace openport;
namespace fs = std::filesystem;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

class StaticFiles : public testing::Test {
 protected:
  void SetUp() override {
    parent = fs::temp_directory_path() /
             ("openport-static-" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    root = parent / "root";
    fs::create_directories(root / "assets");
    fs::create_directories(parent / "root-outside");
    std::ofstream(root / "index.html") << "shell";
    std::ofstream(root / "assets" / "app.js") << "asset";
    std::ofstream(parent / "root-outside" / "secret") << "secret";
  }
  void TearDown() override { fs::remove_all(parent); }
  fs::path parent;
  fs::path root;
};

TEST_F(StaticFiles, RejectsAbsoluteTraversalBackslashAndNulComponents) {
  for (const std::string target :
       {"//etc/passwd", "/%2Fetc/passwd", "/a/%2fetc/passwd", "/../secret", "/%2e%2e/secret",
        "/a\\b", "/a%5cb", "/a%00b", "/bad%", "/bad%zz"}) {
    const auto result = server::resolve_static_file(root, target);
    EXPECT_EQ(result.status, 400) << target;
    EXPECT_FALSE(result.reason.empty());
  }
  EXPECT_EQ(server::resolve_static_file(root, std::string("/a") + '\0' + "b").status, 400);
}

TEST_F(StaticFiles, RejectsSymlinksOutsideTheCanonicalRootIncludingTheShell) {
  fs::create_symlink(parent / "root-outside" / "secret", root / "leak.js");
  fs::create_directory_symlink(parent / "root-outside", root / "escape");
  EXPECT_EQ(server::resolve_static_file(root, "/leak.js").status, 403);
  EXPECT_EQ(server::resolve_static_file(root, "/escape/secret").status, 403);
  EXPECT_EQ(server::resolve_static_file(root, "/escape/missing-route").status, 403);
  fs::remove(root / "index.html");
  fs::create_symlink(parent / "root-outside" / "secret", root / "index.html");
  EXPECT_EQ(server::resolve_static_file(root, "/chain/SPX").status, 403);
}

TEST_F(StaticFiles, ServesAssetsAndSpaFallbackButOnlyRegularFiles) {
  EXPECT_EQ(server::resolve_static_file(root, "/assets/app.js?v=1").path,
            fs::canonical(root / "assets" / "app.js"));
  EXPECT_EQ(server::resolve_static_file(root, "/assets/%61pp.js").status, 200);
  EXPECT_EQ(server::resolve_static_file(root, "/chain/SPX").path,
            fs::canonical(root / "index.html"));
  EXPECT_EQ(server::resolve_static_file(root, "/").status, 200);
  EXPECT_EQ(server::resolve_static_file(root, "/assets/missing.js").status, 404);
  EXPECT_EQ(server::resolve_static_file(root, "/assets").status, 404);
  fs::create_symlink(root / "assets" / "app.js", root / "local.js");
  EXPECT_EQ(server::resolve_static_file(root, "/local.js").status, 200);
}

TEST(WebPolicy, WebSocketOriginsMustMatchHost) {
  EXPECT_TRUE(server::websocket_origin_allowed(std::nullopt, "localhost:8080"));
  EXPECT_TRUE(server::websocket_origin_allowed("http://localhost:8080", "localhost:8080"));
  EXPECT_TRUE(server::websocket_origin_allowed("http://LOCALHOST:80", "localhost"));
  EXPECT_TRUE(server::websocket_origin_allowed("http://[::1]:8080", "[::1]:8080"));
  for (const std::string origin :
       {"", "null", "https://evil.test", "http://localhost:8081", "http://localhost:8080@evil.test",
        "http://localhost:8080/", "http://localhost:8080,https://evil.test",
        "http://localhost:8080?x"}) {
    EXPECT_FALSE(server::websocket_origin_allowed(origin, "localhost:8080")) << origin;
  }
}

TEST(WebPolicy, HostsMustBeAddressesLocalhostOrNamesAllowedOnPurpose) {
  for (const std::string host : {"127.0.0.1", "127.0.0.1:8080", "10.0.0.7:8080", "[::1]:8080", "[fe80::1]",
                                 "localhost", "LOCALHOST:8080", "app.localhost:5173"}) {
    EXPECT_TRUE(server::host_allowed(host)) << host;
  }
  // DNS rebinding arrives under the attacker's name, and malformed values fail closed.
  for (const std::string host : {"evil.test", "evil.test:8080", "localhost.evil.test", "127.0.0.1.evil.test",
                                 "", "localhost:8080@evil.test", "localhost:99999", "[::1", "local host"}) {
    EXPECT_FALSE(server::host_allowed(host)) << host;
  }
  EXPECT_TRUE(server::host_allowed("Terminal.Example", {"https://terminal.example"}));
  EXPECT_TRUE(server::host_allowed("internal:8080", {}, {"internal"}));
  EXPECT_TRUE(server::host_allowed("internal", {}, {"INTERNAL:9000"}));
  EXPECT_FALSE(server::host_allowed("terminal.example.evil", {"https://terminal.example"}, {"internal"}));
}

TEST(WebPolicy, SessionSlotsAreBoundedAndReleasedAfterFailedOrClosedSessions) {
  server::WebSocketSlots slots;
  std::vector<std::unique_ptr<server::WebSocketSlots::Lease>> sessions;
  for (std::size_t i = 0; i < 256; ++i) {
    auto session = slots.acquire();
    ASSERT_TRUE(session);
    sessions.push_back(std::move(session));
  }
  EXPECT_FALSE(slots.acquire());
  sessions.pop_back();
  EXPECT_TRUE(slots.acquire());
  sessions.clear();
  EXPECT_TRUE(slots.acquire());
}

TEST(WebServer, RejectsForeignOriginsAndOmitsCorsHeaders) {
  server::WebServer web("127.0.0.1", 0, {},
                        [](const auto&) { return server::ApiResponse{200, "{}"}; });
  web.start();
  asio::io_context io;
  const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), web.port());
  const auto host = "127.0.0.1:" + std::to_string(web.port());
  tcp::socket socket(io);
  socket.connect(endpoint);
  http::request<http::empty_body> request{http::verb::get, "/api/status", 11};
  request.set(http::field::host, host);
  http::write(socket, request);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(socket, buffer, response);
  EXPECT_EQ(response.count(http::field::access_control_allow_origin), 0u);
  websocket::stream<tcp::socket> ws(io);
  ws.next_layer().connect(endpoint);
  ws.set_option(websocket::stream_base::decorator(
      [](websocket::request_type& req) { req.set(http::field::origin, "https://evil.test"); }));
  beast::error_code ec;
  websocket::response_type upgrade;
  ws.handshake(upgrade, host, "/ws", ec);
  EXPECT_TRUE(ec);
  EXPECT_EQ(upgrade.result(), http::status::forbidden);
  web.stop();
}

TEST(WebServer, RefusesRequestsAddressedByARebindingName) {
  server::WebServer web("127.0.0.1", 0, {},
                        [](const auto&) { return server::ApiResponse{200, "{}"}; });
  web.start();
  asio::io_context io;
  const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), web.port());
  const auto port = std::to_string(web.port());
  const auto get = [&](const std::string& host, const std::string& target) {
    tcp::socket socket(io);
    socket.connect(endpoint);
    http::request<http::empty_body> request{http::verb::get, target, 11};
    request.set(http::field::host, host);
    http::write(socket, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return response;
  };
  EXPECT_EQ(get("127.0.0.1:" + port, "/api/status").result(), http::status::ok);
  EXPECT_EQ(get("localhost:" + port, "/api/status").result(), http::status::ok);
  const auto api = get("evil.test:" + port, "/api/status");
  EXPECT_EQ(api.result(), http::status::forbidden);
  EXPECT_NE(api.body().find("HOST_REJECTED"), std::string::npos) << api.body();
  EXPECT_EQ(get("evil.test:" + port, "/").result(), http::status::forbidden);
  // A rebinding page is same-origin with its own name, so its Origin matches Host.
  websocket::stream<tcp::socket> ws(io);
  ws.next_layer().connect(endpoint);
  ws.set_option(websocket::stream_base::decorator(
      [&](websocket::request_type& req) { req.set(http::field::origin, "http://evil.test:" + port); }));
  beast::error_code ec;
  websocket::response_type upgrade;
  ws.handshake(upgrade, "evil.test:" + port, "/ws", ec);
  EXPECT_TRUE(ec);
  EXPECT_EQ(upgrade.result(), http::status::forbidden);
  web.stop();
}

TEST(WebServer, ClosesWebSocketsWithMessagesLargerThanFourKiB) {
  server::WebServer web("127.0.0.1", 0, {}, [](const auto&) { return server::ApiResponse{}; });
  web.start();
  asio::io_context io;
  websocket::stream<tcp::socket> ws(io);
  ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), web.port()});
  ws.handshake("127.0.0.1:" + std::to_string(web.port()), "/ws");
  ws.write(asio::buffer(std::string(4097, 'x')));
  beast::flat_buffer buffer;
  beast::error_code ec;
  bool completed = false;
  ws.async_read(buffer, [&](beast::error_code error, std::size_t) {
    ec = error;
    completed = true;
  });
  io.run_for(std::chrono::minutes(5));
  EXPECT_TRUE(completed);
  EXPECT_EQ(ec, websocket::error::closed);
  EXPECT_EQ(ws.reason().code, websocket::close_code::too_big);
  web.stop();
}

}  // namespace

namespace {
TEST(WebPolicy, ExplicitProxyOriginsMatchExactlyAfterNormalization) {
  const std::vector<std::string> allowed{"https://Terminal.Example:443", "http://localhost:3000"};
  EXPECT_TRUE(
      server::websocket_origin_allowed("https://terminal.example", "internal:8080", allowed));
  EXPECT_TRUE(server::websocket_origin_allowed("http://localhost:3000", "internal:8080", allowed));
  EXPECT_TRUE(server::websocket_origin_allowed("http://internal:8080", "internal:8080", allowed));
  for (const auto* origin :
       {"http://terminal.example", "https://terminal.example:444", "https://terminal.example.evil",
        "https://terminal.example/", "https://terminal.example@evil", "null"})
    EXPECT_FALSE(server::websocket_origin_allowed(origin, "internal:8080", allowed)) << origin;
  EXPECT_THROW((server::WebServer("127.0.0.1", 0, {}, {}, {"https://host/path"})),
               std::invalid_argument);
}

TEST(WebPolicy, SlowClientKeepsActiveWriteAndOnlyNewestPendingTick) {
  server::TickQueue queue;
  const auto active = std::make_shared<const std::string>("active");
  queue.push(active);
  for (int i = 0; i < 10000; ++i) {
    queue.push(std::make_shared<const std::string>(std::to_string(i)));
    EXPECT_EQ(queue.size(), 2u);
    EXPECT_EQ(queue.front(), active);
  }
  queue.pop();
  ASSERT_EQ(queue.size(), 1u);
  EXPECT_EQ(*queue.front(), "9999");
  queue.pop();
  EXPECT_TRUE(queue.empty());
}

TEST_F(StaticFiles, MissingWebRootExplainsHowToBuildOrSelectTheTerminal) {
  const auto result = server::resolve_static_file(parent / "missing", "/");
  EXPECT_EQ(result.status, 404);
  EXPECT_EQ(result.reason,
            "web terminal not built: run `npm run build` in web/, or pass --web-root");
}

TEST(WebServer, StopClosesHttpAndWebSocketSessionsAndReleasesThePort) {
  server::WebServer web("127.0.0.1", 0, {}, [](const auto&) { return server::ApiResponse{}; });
  web.start(2);
  const auto port = web.port();
  asio::io_context io;
  websocket::stream<tcp::socket> ws(io);
  ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), port});
  ws.handshake("127.0.0.1:" + std::to_string(port), "/ws");
  tcp::socket http_socket(io);
  http_socket.connect({asio::ip::make_address("127.0.0.1"), port});
  // Round-trip one request so the server is known to own this keep-alive session.
  http::request<http::empty_body> request{http::verb::get, "/api/status", 11};
  request.set(http::field::host, "localhost");
  http::write(http_socket, request);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(http_socket, buffer, response);
  web.broadcast(std::string(1024 * 1024, 'x'));
  const auto started = std::chrono::steady_clock::now();
  web.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
  EXPECT_THROW(web.start(), std::logic_error);
  bool http_closed = false, ws_closed = false;
  char byte;
  http_socket.async_read_some(asio::buffer(&byte, 1),
                              [&](auto ec, auto) { http_closed = bool(ec); });
  beast::flat_buffer ws_buffer;
  std::function<void()> read_ws;
  read_ws = [&] {
    ws.async_read(ws_buffer, [&](auto ec, auto) {
      if (ec)
        ws_closed = true;
      else {
        ws_buffer.consume(ws_buffer.size());
        read_ws();
      }
    });
  };
  read_ws();
  io.run_for(std::chrono::minutes(5));
  EXPECT_TRUE(http_closed);
  EXPECT_TRUE(ws_closed);
  server::WebServer replacement("127.0.0.1", port, {},
                                [](const auto&) { return server::ApiResponse{}; });
  EXPECT_NO_THROW(replacement.start());
  replacement.stop();
}

TEST(WebServer, RejectsSecondStartWhileRunningAndAcceptsConfiguredProxyOrigin) {
  server::WebServer web("127.0.0.1", 0, {}, [](const auto&) { return server::ApiResponse{}; },
                        {"https://terminal.example"}, {}, {"internal"});
  web.start();
  EXPECT_THROW(web.start(), std::logic_error);
  asio::io_context io;
  websocket::stream<tcp::socket> ws(io);
  ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), web.port()});
  ws.set_option(websocket::stream_base::decorator(
      [](auto& request) { request.set(http::field::origin, "https://terminal.example:443"); }));
  EXPECT_NO_THROW(ws.handshake("internal:8080", "/ws"));
  web.stop();
}
}  // namespace

TEST(WebPolicy, FailedWebServerStartStillConsumesTheSingleStartAttempt) {
  openport::server::WebServer web("invalid-address", 0, {}, {});
  EXPECT_THROW(web.start(), std::exception);
  EXPECT_THROW(web.start(), std::logic_error);
  EXPECT_NO_THROW(web.stop());
}

namespace {
TEST(WebServer, AsyncPostDeleteRoundTripsAndSecurityHeaders) {
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<server::ApiCompletion> pending;
  std::optional<server::ApiRequest> received;
  server::WebServer web("127.0.0.1", 0, {},
      [&](const server::ApiRequest& request, server::ApiCompletion complete) {
        if (request.method == "GET") { complete({200, "{}"}); return; }
        const std::lock_guard lock(mutex);
        received = request; pending = std::move(complete); ready.notify_one();
      }, {}, "secret");
  web.start(1);
  asio::io_context io;
  beast::tcp_stream connection(io);
  connection.expires_after(std::chrono::minutes(5));
  connection.connect({asio::ip::make_address("127.0.0.1"), web.port()});
  const auto host = "127.0.0.1:" + std::to_string(web.port());
  auto request = [&](http::verb method, const std::string& target, const std::string& body) {
    http::request<http::string_body> out{method, target, 11};
    out.set(http::field::host, host); out.set(http::field::origin, "http://" + host);
    out.set(http::field::authorization, "Bearer secret");
    if (method != http::verb::delete_) out.set(http::field::content_type, "application/json");
    out.body() = body; out.prepare_payload(); return out;
  };
  for (const auto method : {http::verb::post, http::verb::delete_}) {
    const auto body = method == http::verb::post ? "{\"client_order_id\":\"socket\"}" : "";
    auto req = request(method, method == http::verb::post ? "/api/orders" : "/api/orders/1", body);
    http::write(connection, req);
    server::ApiCompletion complete;
    {
      std::unique_lock lock(mutex);
      ASSERT_TRUE(ready.wait_for(lock, std::chrono::minutes(5), [&] { return pending.has_value(); }));
      EXPECT_EQ(received->body, body);
      EXPECT_EQ(received->authorization, "Bearer secret");
      complete = std::move(*pending); pending.reset();
    }
    // A second request completes on the single I/O thread while the first is
    // waiting for its owner-thread callback. A blocking handler would deadlock.
    beast::tcp_stream other(io);
    other.expires_after(std::chrono::minutes(5));
    other.connect({asio::ip::make_address("127.0.0.1"), web.port()});
    auto get = request(http::verb::get, "/api/status", "");
    http::write(other, get);
    beast::flat_buffer other_buffer;
    http::response<http::string_body> other_response;
    http::read(other, other_buffer, other_response);
    EXPECT_EQ(other_response.result_int(), 200);
    std::thread engine([complete = std::move(complete), method]() mutable {
      complete({method == http::verb::post ? 201 : 200, "{\"account_version\":\"3\"}"});
    });
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(connection, buffer, response);
    engine.join();
    EXPECT_EQ(response.result_int(), method == http::verb::post ? 201 : 200);
    EXPECT_EQ(response.body(), "{\"account_version\":\"3\"}");
  }
  auto rejected = request(http::verb::post, "/api/orders", "{}");
  rejected.erase(http::field::authorization);
  http::write(connection, rejected);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(connection, buffer, response);
  EXPECT_EQ(response.result_int(), 403);
  EXPECT_NE(response.body().find("WRITE_TOKEN_REQUIRED"), std::string::npos);
  web.stop();
}

TEST(WebServer, StopSeversLateCommandCompletionBeforeDestroyingExecutor) {
  server::ApiCompletion complete;
  std::promise<void> received;
  asio::io_context io;
  beast::tcp_stream connection(io);
  {
    server::WebServer web("127.0.0.1", 0, {},
        [&](const server::ApiRequest&, server::ApiCompletion callback) {
          complete = std::move(callback); received.set_value();
        });
    web.start();
    connection.connect({asio::ip::make_address("127.0.0.1"), web.port()});
    http::request<http::string_body> request{http::verb::post, "/api/risk/kill", 11};
    request.set(http::field::host, "localhost");
    request.set(http::field::content_type, "application/json");
    request.body() = "{}"; request.prepare_payload();
    http::write(connection, request);
    ASSERT_EQ(received.get_future().wait_for(std::chrono::minutes(5)), std::future_status::ready);
    web.stop();
  }
  EXPECT_NO_THROW(complete({200, "{}"}));
}
}  // namespace

namespace {
TEST(WebPolicy, NamedTokenFilesAreStrictAndDiagnosticsHideSecrets) {
  const auto tokens = server::parse_token_file("# agents\nreader read read-secret\nagent read,trade:practice,replay agent-secret # local\nowner admin admin-secret\n");
  ASSERT_EQ(tokens.size(), 3U);
  EXPECT_EQ(tokens[1].name, "agent");
  EXPECT_EQ(tokens[1].scopes, (std::vector<std::string>{"read", "trade:practice", "replay"}));
  for (const std::string line : {"", "# empty", "a read", "a read secret extra", "a trade: secret", "a read, secret",
      "a unknown secret", "a read,read secret", "system admin secret", "loopback admin secret", "a read secret\na admin other",
      "a read secret\nb admin secret", "a trade:../main secret", "a trade:MAIN secret", "a trade:bad_id secret", "a trade:-main secret",
      "a trade:main--two secret", "a read secret\nb ,read other"}) {
    try { (void)server::parse_token_file(line); FAIL() << "Accepted malformed token file"; }
    catch (const std::invalid_argument& error) {
      EXPECT_EQ(std::string(error.what()).find("secret"), std::string::npos);
    }
  }
}

TEST(WebPolicy, NamedTokensEnforceEveryRouteFamilyAndAccount) {
  server::WritePolicy policy{"0.0.0.0", "legacy-secret", {},
      server::parse_token_file("reader read reader-secret\nagent trade:practice agent-secret\nall trade:* all-secret\nreplayer replay replay-secret\nowner admin owner-secret"), true};
  const auto check = [&](std::string method, std::string path, const std::string& secret) {
    server::ApiRequest request{std::move(method), std::move(path)};
    request.content_type = "application/json";
    request.authorization = "Bearer " + secret;
    return server::check_api_write(request, policy);
  };
  for (const auto* path : {"/api/status", "/api/accounts", "/api/portfolio?account=practice", "/api/replay", "/ws"}) {
    EXPECT_FALSE(check("GET", path, "reader-secret"));
    EXPECT_TRUE(check("GET", path, "agent-secret"));
    EXPECT_FALSE(check("GET", path, "owner-secret"));
  }
  const std::vector<std::pair<std::string, std::string>> trades = {
      {"POST", "/orders"}, {"POST", "/orders/preview"}, {"PUT", "/orders/1"}, {"DELETE", "/orders/1"},
      {"POST", "/orders/cancel"}, {"POST", "/positions/close"}, {"POST", "/positions/exercise"},
      {"POST", "/stocks/close"}, {"PUT", "/trades/1/note"}, {"PUT", "/days/2026-09-22/note"},
      {"POST", "/playbooks/staged/12/send"}, {"POST", "/playbooks/staged/12/dismiss"}};
  for (const auto& [method, path] : trades) {
    EXPECT_FALSE(check(method, "/api" + path + "?account=practice", "agent-secret")) << path;
    EXPECT_TRUE(check(method, "/api" + path, "agent-secret")) << path;
    EXPECT_TRUE(check(method, "/api" + path + "?account=other", "agent-secret")) << path;
    EXPECT_FALSE(check(method, "/api" + path + "?account=other", "all-secret")) << path;
    EXPECT_TRUE(check(method, "/api/replay" + path, "all-secret")) << path;
    EXPECT_FALSE(check(method, "/api/replay" + path, "replay-secret")) << path;
    EXPECT_TRUE(check(method, "/api" + path, "replay-secret")) << path;
    EXPECT_TRUE(check(method, "/api" + path, "reader-secret")) << path;
  }
  for (const std::string method : {"POST", "PUT", "DELETE"}) {
    EXPECT_FALSE(check(method, "/api/replay", "replay-secret"));
    EXPECT_TRUE(check(method, "/api/replay", "agent-secret"));
  }
  for (const auto* path : {"/api/risk/limits", "/api/risk/guardrails", "/api/risk/kill", "/api/account/reset",
       "/api/account/payout", "/api/accounts", "/api/settlements", "/api/replay/risk/limits", "/api/replay/history/run", "/api/future-write",
       "/api/playbooks", "/api/playbooks/morning/mode"}) {
    EXPECT_TRUE(check("POST", path, "agent-secret")) << path;
    EXPECT_TRUE(check("POST", path, "replay-secret")) << path;
    EXPECT_FALSE(check("POST", path, "owner-secret")) << path;
    EXPECT_FALSE(check("POST", path, "legacy-secret")) << path;
  }
}

TEST(WebPolicy, EverySpellingOfTheAccountIsScopedAsTheRoutesReadIt) {
  // Routes percent-decode query keys and values (%61 is "a"), so the scope check reads
  // the account through the same parser and cannot be sidestepped by encoding it.
  server::WritePolicy policy{"0.0.0.0", "", {}, server::parse_token_file("mainer trade:main mainer-secret\nbeta trade:beta beta-secret"), false};
  const auto check = [&](std::string method, std::string path, const std::string& secret) {
    server::ApiRequest request{std::move(method), std::move(path)};
    request.content_type = "application/json";
    request.authorization = "Bearer " + secret;
    return server::check_api_write(request, policy);
  };
  for (const std::string path : {"/api/playbooks/staged/1/send", "/api/orders", "/api/positions/close"}) {
    for (const auto* query : {"?account=beta", "?%61ccount=beta", "?%61%63%63ount=beta", "?account=%62eta", "?%61ccount=%62%65%74%61", "?x=1&%61ccount=beta"}) {
      const auto refused = check("POST", path + query, "mainer-secret");
      ASSERT_TRUE(refused) << path << query;
      EXPECT_EQ(refused->status, 403) << path << query;
      EXPECT_FALSE(check("POST", path + query, "beta-secret")) << path << query;
    }
    // Two spellings of one key are ambiguous whichever comes first, and a malformed
    // query or account cannot fall back to the main account.
    for (const auto* query : {"?account=main&%61ccount=beta", "?%61ccount=beta&account=main", "?account=main&account=beta",
                              "?%zz=1&account=main", "?account=%6", "?account=Beta", "?account=", "?account"}) {
      const auto refused = check("POST", path + query, "mainer-secret");
      ASSERT_TRUE(refused) << path << query;
      EXPECT_EQ(refused->status, 400) << path << query;
    }
    EXPECT_FALSE(check("POST", path + "?%61ccount=main", "mainer-secret")) << path;
  }
  EXPECT_EQ(server::query_account("/api/orders?%61ccount=%62eta"), "beta");
  EXPECT_EQ(server::query_account("/api/orders"), "main");
  EXPECT_EQ(server::query_account("/api/orders?status=open"), "main");
  EXPECT_FALSE(server::query_account("/api/orders?account=main&%61ccount=main"));
  const auto form = server::query_parameters("template=%7B%22name%22%3A%22iron+condor%22%7D&flag", true);
  ASSERT_TRUE(form);
  EXPECT_EQ(form->at("template"), R"({"name":"iron condor"})");
  EXPECT_EQ(form->at("flag"), "");
  EXPECT_EQ(server::query_parameters("from=2026-09-28T09:30:00+00:00")->at("from"), "2026-09-28T09:30:00+00:00");
  EXPECT_EQ(server::query_parameters("from=2026-09-28T13%3A30%3A00Z")->at("from"), "2026-09-28T13:30:00Z");
}

TEST(WebPolicy, WriteModeSaysWhetherAWriteWithoutCredentialsIsAccepted) {
  const auto tokens = server::parse_token_file("reader read reader-secret");
  server::Sandboxes::Options limits;
  limits.capacity = 1;
  const auto sandboxes = std::make_shared<server::Sandboxes>(limits);
  server::ApiRequest order{"POST", "/api/orders"};
  order.content_type = "application/json";
  const std::vector<std::pair<server::WritePolicy, std::string>> cases{
      {{"127.0.0.1", "", {}}, "open"},
      {{"127.0.0.1", "", {}, tokens}, "open"},  // named tokens alone leave loopback writes open
      {{"127.0.0.1", "legacy-secret", {}}, "token"},
      {{"127.0.0.1", "", {}, tokens, true}, "token"},
      {{"127.0.0.1", "", {}, {}, false, sandboxes}, "token"},
      {{"0.0.0.0", "", {}}, "disabled"},
      {{"0.0.0.0", "", {}, tokens}, "token"},
      {{"0.0.0.0", "legacy-secret", {}}, "token"}};
  for (const auto& [policy, mode] : cases) {
    EXPECT_EQ(server::write_mode(policy), mode) << policy.address << " token=" << policy.token << " named=" << policy.tokens.size();
    // A terminal reading "open" writes without a token; any other mode needs one or refuses.
    EXPECT_EQ(!server::check_api_write(order, policy), mode == "open") << policy.address << " " << mode;
  }
}

TEST(WebPolicy, RequireTokenProtectsLoopbackReadsAndWritesAndAttributesActors) {
  server::WritePolicy policy{"127.0.0.1", "", {}, server::parse_token_file("alice read,trade:main secret"), false};
  server::ApiRequest request{"POST", "/api/orders"};
  request.content_type = "application/json";
  std::string actor;
  EXPECT_FALSE(server::check_api_write(request, policy, &actor));
  EXPECT_EQ(actor, "loopback");
  policy.require_token = true;
  EXPECT_TRUE(server::check_api_write(request, policy));
  request.authorization = "Bearer secret";
  EXPECT_FALSE(server::check_api_write(request, policy, &actor));
  EXPECT_EQ(actor, "alice");
  request.method = "GET";
  request.target = "/api/status";
  EXPECT_FALSE(server::check_api_write(request, policy));
  request.authorization.clear();
  EXPECT_TRUE(server::check_api_write(request, policy));
  request.authorization = "Bearer wrong";
  EXPECT_TRUE(server::check_api_write(request, policy));
  // Without --require-token reads stay public: a stale or narrow token still reads,
  // and only its writes are refused.
  policy.require_token = false;
  EXPECT_FALSE(server::check_api_write(request, policy));
  request.authorization.clear();
  EXPECT_FALSE(server::check_api_write(request, policy));
  request.method = "POST";
  request.target = "/api/orders";
  request.authorization = "Bearer wrong";
  EXPECT_TRUE(server::check_api_write(request, policy));
  policy.tokens = server::parse_token_file("agent trade:practice agent-secret");
  request.method = "GET";
  request.target = "/api/portfolio";
  request.authorization = "Bearer agent-secret";
  EXPECT_FALSE(server::check_api_write(request, policy));
}

TEST(WebPolicy, BrowserSocketTokensUseHeadersWithoutAmbiguousCredentials) {
  EXPECT_EQ(server::websocket_authorization("", "openport, openport.token.616263"), "Bearer abc");
  EXPECT_EQ(server::websocket_authorization("Bearer abc", ""), "Bearer abc");
  EXPECT_FALSE(server::websocket_authorization("Bearer abc", "openport, openport.token.616263"));
  for (const auto* value : {"openport, openport.token.", "openport, openport.token.0", "openport, openport.token.00", "openport, openport.token.gg", "other"}) {
    EXPECT_FALSE(server::websocket_authorization("", value));
  }
}
}  // namespace

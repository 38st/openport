#include "openport/server/web_policy.hpp"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <fstream>
#include <thread>
#include <vector>

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
  io.run_for(std::chrono::seconds(3));
  EXPECT_TRUE(completed);
  EXPECT_EQ(ec, websocket::error::closed);
  EXPECT_EQ(ws.reason().code, websocket::close_code::too_big);
  web.stop();
}

}  // namespace

#include "openport/net/http.hpp"

#include <gtest/gtest.h>
#include <zlib.h>

#include <stdexcept>
#include <string>

namespace {

using openport::net::gunzip;
using openport::net::parse_url;

std::string gzip(const std::string& text) {
  z_stream zs{};
  EXPECT_EQ(deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY),
            Z_OK);
  std::string out(deflateBound(&zs, static_cast<uLong>(text.size())) + 32, '\0');
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));
  zs.avail_in = static_cast<uInt>(text.size());
  zs.next_out = reinterpret_cast<Bytef*>(out.data());
  zs.avail_out = static_cast<uInt>(out.size());
  EXPECT_EQ(deflate(&zs, Z_FINISH), Z_STREAM_END);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

TEST(Http, GunzipRoundTripsLargeRepetitiveText) {
  std::string text;
  for (int i = 0; i < 200000; ++i) text += "{\"option\":\"SPXW261005P07405000\",\"bid\":4.4},";
  EXPECT_EQ(gunzip(gzip(text)), text);
}

TEST(Http, GunzipRejectsCorruptInput) {
  std::string compressed = gzip("hello, options");
  compressed.resize(compressed.size() / 2);
  EXPECT_THROW((void)gunzip(compressed), std::runtime_error);
  EXPECT_THROW((void)gunzip("not gzip at all"), std::runtime_error);
}

TEST(Http, GunzipRejectsExpansionBeyond256MiB) {
  const auto compressed = gzip(std::string(256 * 1024 * 1024 + 1, 'x'));
  EXPECT_THROW((void)gunzip(compressed), std::runtime_error);
}

TEST(Http, GunzipAcceptsExactlyTheDecompressionLimit) {
  EXPECT_EQ(openport::net::kMaxDecompressedBytes, 256u * 1024 * 1024);
  const auto compressed = gzip(std::string(openport::net::kMaxDecompressedBytes, 'x'));
  EXPECT_EQ(gunzip(compressed).size(), openport::net::kMaxDecompressedBytes);
}

TEST(Http, ParsesUrls) {
  const auto url = parse_url("https://cdn.cboe.com/api/global/delayed_quotes/options/_SPX.json");
  ASSERT_TRUE(url);
  EXPECT_TRUE(url->tls);
  EXPECT_EQ(url->host, "cdn.cboe.com");
  EXPECT_EQ(url->port, "443");
  EXPECT_EQ(url->target, "/api/global/delayed_quotes/options/_SPX.json");

  const auto with_port = parse_url("https://localhost:8443?a=1");
  ASSERT_TRUE(with_port);
  EXPECT_EQ(with_port->host, "localhost");
  EXPECT_EQ(with_port->port, "8443");
  EXPECT_EQ(with_port->target, "/?a=1");

  const auto local = parse_url("http://127.0.0.1:25503/v3/option/snapshot/quote?symbol=SPY");
  ASSERT_TRUE(local);
  EXPECT_FALSE(local->tls);
  EXPECT_EQ(local->port, "25503");
  EXPECT_EQ(local->target, "/v3/option/snapshot/quote?symbol=SPY");
  EXPECT_EQ(parse_url("http://example.com")->port, "80");

  EXPECT_FALSE(parse_url("ftp://example.com/"));
  EXPECT_FALSE(parse_url("https://"));
}

}  // namespace

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <future>

#include "openport/md/event_queue.hpp"
#include "openport/providers/massive.hpp"

namespace {
TEST(Http, PreCancelledRequestsFailBeforeDnsOrNetworkAccess) {
  openport::net::HttpClient client;
  std::atomic<bool> cancel{true};
  const auto started = std::chrono::steady_clock::now();
  try {
    client.get("https://does-not-exist.invalid/", {}, std::chrono::seconds(30), &cancel);
    FAIL() << "cancelled request succeeded";
  } catch (const std::exception& error) {
    EXPECT_NE(std::string(error.what()).find("cancelled"), std::string::npos);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
}

// Socket tests use the WebServer suite so the sandbox-safe filter excludes them.
TEST(WebServer, HttpCancellationInterruptsReadAndTlsHandshake) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  for (const auto* scheme : {"http://", "https://"}) {
    asio::io_context io;
    tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
    tcp::socket peer(io);
    std::atomic<bool> cancel{false};
    openport::net::HttpClient client;
    const auto url =
        std::string(scheme) + "127.0.0.1:" + std::to_string(listener.local_endpoint().port());
    auto request = std::async(std::launch::async, [&] {
      try {
        client.get(url, {}, std::chrono::seconds(30), &cancel);
        return std::string("succeeded");
      } catch (const std::exception& error) {
        return std::string(error.what());
      }
    });
    bool received = false;
    char bytes[4096];
    listener.async_accept(peer, [&](auto ec) {
      ASSERT_FALSE(ec);
      peer.async_read_some(asio::buffer(bytes), [&](auto error, auto) {
        EXPECT_FALSE(error);
        received = true;
      });
    });
    io.run_for(std::chrono::seconds(3));
    EXPECT_TRUE(received);  // HTTP request or TLS ClientHello, deliberately unanswered
    const auto started = std::chrono::steady_clock::now();
    cancel = true;
    EXPECT_EQ(request.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_NE(request.get().find("cancelled"), std::string::npos);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
  }
}

TEST(WebServer, PollingProviderStopInterruptsAnInFlightHttpRead) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  tcp::socket peer(io);
  openport::providers::MassiveProvider provider(
      {.api_key = "test",
       .base_url = "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port())});
  openport::md::EventQueue queue;
  bool received = false;
  char bytes[4096];
  listener.async_accept(peer, [&](auto ec) {
    ASSERT_FALSE(ec);
    peer.async_read_some(asio::buffer(bytes), [&](auto error, auto) {
      EXPECT_FALSE(error);
      received = true;
    });
  });
  provider.start({{"SPY"}}, queue);
  io.run_for(std::chrono::seconds(3));
  EXPECT_TRUE(received);
  const auto started = std::chrono::steady_clock::now();
  provider.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
}
}  // namespace

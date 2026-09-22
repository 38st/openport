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

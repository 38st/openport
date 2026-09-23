#include "openport/server/api.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>

#include "openport/analytics/chain_analytics.hpp"
#include "openport/net/http.hpp"
#include "openport/server/web_server.hpp"
#include "support/synthetic_chain.hpp"

namespace {

using namespace openport;
using nlohmann::json;

/// Serves analytics computed once from the synthetic chain.
class StubSource final : public server::MetricsSource {
 public:
  explicit StubSource(analytics::UnderlyingMetrics metrics)
      : metrics_(std::make_shared<const analytics::UnderlyingMetrics>(std::move(metrics))) {}

  StubSource() {
    test::SyntheticChain chain;
    metrics_ = std::make_shared<const analytics::UnderlyingMetrics>(
        analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of));
    status_.provider = "cboe";
    status_.capabilities.delay = std::chrono::seconds(900);
    status_.feed_state = md::FeedState::Delayed;
    status_.feed_message = "cboe SPX: 82 options";
  }

  std::vector<std::string> symbols() const override { return {metrics_->symbol}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string& symbol) const override {
    return symbol == metrics_->symbol ? metrics_ : nullptr;
  }
  server::EngineStatus status() const override { return status_; }

 private:
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics_;
  server::EngineStatus status_;
};

json get(const StubSource& source, const std::string& target, int expected_status = 200) {
  const server::ApiResponse response = server::handle_api({"GET", target}, source);
  EXPECT_EQ(response.status, expected_status) << target << " -> " << response.body;
  return json::parse(response.body);
}

TEST(Api, StatusDescribesProviderFeedAndUnderlyings) {
  StubSource source;
  const json status = get(source, "/api/status");
  EXPECT_EQ(status["provider"]["name"], "cboe");
  EXPECT_EQ(status["provider"]["delay_seconds"], 900);
  EXPECT_EQ(status["feed"]["state"], "delayed");
  ASSERT_EQ(status["underlyings"].size(), 1u);
  EXPECT_EQ(status["underlyings"][0]["symbol"], "SPX");
  EXPECT_DOUBLE_EQ(status["underlyings"][0]["spot"].get<double>(), 5000.0);
}

TEST(Api, SummaryListsExpiriesWithSettlementInTheirIds) {
  StubSource source;
  const json summary = get(source, "/api/underlyings/SPX/summary");
  ASSERT_EQ(summary["expiries"].size(), 1u);
  const json& expiry = summary["expiries"][0];
  EXPECT_EQ(expiry["id"], "2026-10-22PM");
  EXPECT_EQ(expiry["settlement"], "PM");
  EXPECT_EQ(expiry["strikes"], 41);
  EXPECT_NEAR(expiry["rate"].get<double>(), test::SyntheticChain::kRate, 1e-3);
  EXPECT_TRUE(summary["exposure"]["gamma_flip"].is_number());
}

TEST(Api, ChainReturnsBothSidesOfEveryStrike) {
  StubSource source;
  const json chain = get(source, "/api/underlyings/SPX/chain?expiry=2026-10-22PM");
  ASSERT_EQ(chain["strikes"].size(), 41u);
  const json& atm = chain["strikes"][20];
  EXPECT_DOUBLE_EQ(atm["strike"].get<double>(), 5000.0);
  EXPECT_TRUE(atm["call"]["iv"].is_number());
  EXPECT_TRUE(atm["put"]["delta"].is_number());
  EXPECT_TRUE(atm["call"]["vendor_iv"].is_null());  // the synthetic feed sends none

  const json narrow = get(source, "/api/underlyings/SPX/chain?expiry=2026-10-22PM&window=0.02");
  EXPECT_EQ(narrow["strikes"].size(), 9u);  // 4900..5100
}

TEST(Api, ExposureMatrixIsAlignedByStrike) {
  StubSource source;
  const json exposure = get(source, "/api/underlyings/SPX/exposure?window=0.05");
  const auto strikes = exposure["strikes"].get<std::vector<double>>();
  ASSERT_EQ(exposure["expiries"].size(), 1u);
  EXPECT_EQ(exposure["expiries"][0]["gex"].size(), strikes.size());
  EXPECT_EQ(exposure["total_gex"].size(), strikes.size());
}

TEST(Api, SurfaceHasLogMoneynessPoints) {
  StubSource source;
  const json surface = get(source, "/api/underlyings/SPX/surface");
  const json& points = surface["expiries"][0]["points"];
  ASSERT_FALSE(points.empty());
  EXPECT_TRUE(points[0]["k"].is_number());
}

TEST(Api, ErrorsAreJson) {
  StubSource source;
  EXPECT_TRUE(get(source, "/api/underlyings/QQQ/summary", 404).contains("error"));
  EXPECT_TRUE(get(source, "/api/underlyings/SPX/chain?expiry=2030-01-01PM", 404).contains("error"));
  EXPECT_TRUE(get(source, "/api/nope", 404).contains("error"));
  const auto post = server::handle_api({"POST", "/api/status"}, source);
  EXPECT_EQ(post.status, 405);
}

TEST(Api, RejectsMalformedAndOutOfRangeNumbers) {
  StubSource source;
  for (const std::string value : {"", "0", "501", "-1", "1.5", "1e2", "nan", "inf",
                                  "999999999999999999999999", "8junk", " 8"}) {
    EXPECT_TRUE(
        get(source, "/api/underlyings/SPX/exposure?expiries=" + value, 400).contains("error"));
  }
  for (const std::string value : {"", "-0.1", "1.01", "nan", "inf", "1e999", "0.1junk", " 0.1"}) {
    EXPECT_TRUE(get(source, "/api/underlyings/SPX/chain?window=" + value, 400).contains("error"));
  }
  EXPECT_FALSE(get(source, "/api/underlyings/SPX/surface?expiries=500&window=1").contains("error"));
  EXPECT_FALSE(get(source, "/api/underlyings/SPX/chain?expiries=1&window=0").contains("error"));
  EXPECT_TRUE(get(source, "/api/underlyings/SPX/chain?window=2&window=0.5", 400).contains("error"));
  EXPECT_TRUE(get(source, "/api/underlyings/SPX/chain?window=1e-999", 400).contains("error"));
}

TEST(Api, TickMessageCarriesVersions) {
  StubSource source;
  const json tick = json::parse(server::tick_message(source));
  EXPECT_EQ(tick["type"], "tick");
  EXPECT_EQ(tick["underlyings"][0]["symbol"], "SPX");
  EXPECT_TRUE(tick["underlyings"][0]["version"].is_number());
}

TEST(Api, OexAndXeoHaveDistinctPmExpiryIds) {
  analytics::ChainBook book;
  book.apply(md::ContractDefinition{0, *md::parse_osi("OEX261016C03000000")});
  book.apply(md::ContractDefinition{1, *md::parse_osi("XEO261016C03000000")});
  book.apply(md::UnderlyingQuote{"OEX", 1, 0, 0, 3000});
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 12, 0);
  StubSource source(analytics::analyze(book.underlyings().at("OEX"), book, as_of));
  const auto summary = get(source, "/api/underlyings/OEX/summary");
  ASSERT_EQ(summary["expiries"].size(), 2u);
  std::set<std::string> ids;
  for (const auto& expiry : summary["expiries"]) ids.insert(expiry["id"].get<std::string>());
  EXPECT_EQ(ids, (std::set<std::string>{"2026-10-16PM-OEX", "2026-10-16PM-XEO"}));
  for (const auto& id : ids)
    EXPECT_EQ(get(source, "/api/underlyings/OEX/chain?expiry=" + id)["expiry"]["id"], id);
}

TEST(WebServer, ServesTheApiAndTheAppShellOverHttp) {
  const auto root = std::filesystem::temp_directory_path() / "openport-web-test";
  std::filesystem::create_directories(root / "assets");
  std::ofstream(root / "index.html") << "<!doctype html><title>OpenPort</title>";
  std::ofstream(root / "assets" / "app-1234.js") << "console.log('hi')";

  StubSource source;
  server::WebServer web("127.0.0.1", 0, root,
                        [&source](const server::ApiRequest& r) { return server::handle_api(r, source); });
  web.start(1);
  const std::string base = "http://127.0.0.1:" + std::to_string(web.port());

  net::HttpClient http;
  const auto status = http.get(base + "/api/status");
  EXPECT_EQ(status.status, 200);
  EXPECT_EQ(json::parse(status.body)["provider"]["name"], "cboe");

  const auto shell = http.get(base + "/");
  EXPECT_EQ(shell.status, 200);
  EXPECT_NE(shell.body.find("OpenPort"), std::string::npos);

  const auto route = http.get(base + "/chain/SPX");  // client-side route -> app shell
  EXPECT_EQ(route.status, 200);
  EXPECT_NE(route.body.find("OpenPort"), std::string::npos);

  EXPECT_EQ(http.get(base + "/assets/app-1234.js").status, 200);
  EXPECT_EQ(http.get(base + "/assets/missing.js").status, 404);
  EXPECT_EQ(http.get(base + "/../etc/passwd").status, 400);

  web.stop();
  std::filesystem::remove_all(root);
}

}  // namespace

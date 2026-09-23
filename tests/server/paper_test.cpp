#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

#include "openport/server/api.hpp"
#include "openport/server/web_policy.hpp"
#include "support/scripted_market.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;

class PaperProvider final : public md::Provider {
 public:
  std::string_view name() const noexcept override { return "scripted paper"; }
  md::Capabilities capabilities() const noexcept override {
    md::Capabilities result;
    result.delay = delay;
    return result;
  }
  void start(const md::Subscription&, md::EventSink& out) override { sink = &out; }
  void stop() override {}
  md::EventSink* sink = nullptr;
  std::chrono::seconds delay{0};
};
template <class F> bool wait_for(F predicate) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}
server::ApiResponse write(server::MetricsSource& source, std::string method,
                           std::string target, json body = nullptr) {
  auto promise = std::make_shared<std::promise<server::ApiResponse>>();
  auto future = promise->get_future();
  server::ApiRequest request{std::move(method), std::move(target)};
  if (!body.is_null()) request.body = body.dump();
  request.content_type = "application/json";
  server::handle_api_async(request, source, [promise](auto response) { promise->set_value(std::move(response)); });
  if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    throw std::runtime_error("Engine command did not complete");
  return future.get();
}
json read(const server::MetricsSource& source, std::string path) {
  const auto response = server::handle_api({"GET", std::move(path)}, source);
  EXPECT_EQ(response.status, 200) << response.body;
  return json::parse(response.body);
}
json order(const test::ScriptedMarket& market, std::string client = "one", std::string price = "4.00") {
  return {{"client_order_id", client}, {"symbol", market.symbol()}, {"side", "buy"},
          {"type", "limit"}, {"quantity", 1}, {"limit_price", price}, {"time_in_force", "day"}};
}
server::Engine::Options paper_options() {
  server::Engine::Options options;
  options.analytics_interval = std::chrono::milliseconds(0);
  options.analytics.fallback_rate = 0;
  options.clock = [] { return md::new_york_to_utc({2026, 9, 22}, 10, 0); };
  return options;
}
class PaperEngine : public testing::Test {
 protected:
  void SetUp() override {
    engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, paper_options());
    engine->start();
    ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  }
  void seed(std::string bid = "4.00", std::string ask = "4.20", double size = 10) {
    provider.sink->publish(md::ContractDefinition{0, market.contract});
    quote(std::move(bid), std::move(ask), size);
  }
  void quote(std::string bid = "4.00", std::string ask = "4.20", double size = 10) {
    provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    provider.sink->publish(md::OptionQuote{0, market.time, Money::parse(bid).dollars(), Money::parse(ask).dollars(), size, size});
    ASSERT_TRUE(wait_for([&] {
      const auto metrics = engine->metrics("SPX");
      return metrics && metrics->as_of == market.time && !metrics->slices.empty() &&
             metrics->slices[0].strikes[0].call.ask == Money::parse(ask).dollars();
    }));
  }
  void expect_error(const server::ApiResponse& response, int status, const char* code) {
    EXPECT_EQ(response.status, status) << response.body;
    const auto body = json::parse(response.body);
    ASSERT_TRUE(body.contains("error"));
    EXPECT_EQ(body["error"].size(), 5);
    EXPECT_EQ(body["error"]["code"], code);
    EXPECT_TRUE(body["error"]["message"].is_string());
  }
  PaperProvider provider;
  test::ScriptedMarket market;
  std::unique_ptr<server::Engine> engine;
};

class PaperFeed : public PaperEngine {
 protected:
  void TearDown() override { engine->stop(); }
  void SetUp() override {
    provider.delay = std::chrono::minutes(15);
    auto options = paper_options();
    options.clock = [this] { return wall_now.load(); };
    engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX", "XSP"}}, options);
    engine->start();
    ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  }
  json paper_status() {
    EXPECT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->time >= market.time; }));
    const auto status = read(*engine, "/api/status")["underlyings"][0]["paper"];
    EXPECT_EQ(json::parse(server::tick_message(*engine))["underlyings"][0]["paper"], status);
    return status;
  }
  void expect_gate(std::string client, const char* code, const char* message = nullptr) {
    const auto paper = paper_status();
    EXPECT_EQ(paper["accepting"], false);
    EXPECT_EQ(paper["reason"], code);
    if (message) {
      EXPECT_EQ(paper["message"], message);
    }
    const auto response = write(*engine, "POST", "/api/orders", order(market, std::move(client)));
    expect_error(response, 422, code);
    EXPECT_EQ(json::parse(response.body)["error"]["message"], paper["message"]);
    EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["reason"]["code"], code);
  }
  std::atomic<md::Timestamp> wall_now{md::new_york_to_utc({2026, 9, 22}, 10, 15)};
};

TEST_F(PaperFeed, OvernightStallDuringRegularSessionAgreesWithStatusAndTick) {
  market.time = md::new_york_to_utc({2026, 9, 21}, 23, 45);
  wall_now = md::new_york_to_utc({2026, 9, 22}, 10, 5);
  seed();
  EXPECT_EQ(read(*engine, "/api/status")["underlyings"][0]["session"]["name"], "regular");
  expect_gate("stalled", "FEED_STALLED",
              "SPX quotes are 10h 20m behind the market; the feed appears to have stalled");
  EXPECT_TRUE(read(*engine, "/api/fills")["fills"].empty());
  EXPECT_EQ(engine->trading_view()->snapshot->time, market.time);
}

TEST_F(PaperFeed, FirstFifteenMinutesOfDelayedOpenRemainSessionClosed) {
  market.time = md::new_york_to_utc({2026, 9, 22}, 9, 15);
  wall_now = market.time + 15 * md::kNanosPerMinute;
  seed();
  for (int minute = 0; minute < 15; ++minute) {
    market.time = md::new_york_to_utc({2026, 9, 22}, 9, 15 + minute);
    wall_now = market.time + 15 * md::kNanosPerMinute;
    quote();
    expect_gate("opening-" + std::to_string(minute), "SESSION_CLOSED");
  }
  market.time = md::new_york_to_utc({2026, 9, 22}, 9, 30);
  wall_now = market.time + 15 * md::kNanosPerMinute;
  quote();
  EXPECT_EQ(paper_status(), (json{{"accepting", true}, {"reason", nullptr}, {"message", nullptr}}));
  const auto accepted = write(*engine, "POST", "/api/orders", order(market, "regular", "4.20"));
  ASSERT_EQ(accepted.status, 201) << accepted.body;
  EXPECT_EQ(json::parse(accepted.body)["order"]["status"], "filled");
}

TEST_F(PaperFeed, OrdinaryClosedSessionDoesNotReportStall) {
  market.time = md::new_york_to_utc({2026, 9, 21}, 23, 45);
  wall_now = md::new_york_to_utc({2026, 9, 22}, 9, 0);
  seed();
  expect_gate("closed", "SESSION_CLOSED");
}

TEST_F(PaperFeed, AnotherUnderlyingCannotHideAStalledFeed) {
  market.time = md::new_york_to_utc({2026, 9, 21}, 23, 45);
  wall_now = md::new_york_to_utc({2026, 9, 22}, 10, 5);
  seed();
  test::ScriptedMarket fresh;
  fresh.contract = *md::parse_osi("XSP261022C00500000");
  fresh.time = wall_now.load() - 15 * md::kNanosPerMinute;
  provider.sink->publish(md::ContractDefinition{1, fresh.contract});
  provider.sink->publish(md::UnderlyingQuote{"XSP", fresh.time, 500, 500, 500});
  provider.sink->publish(md::OptionQuote{1, fresh.time, 4, 4.2, 10, 10});
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->time == fresh.time; }));
  const auto underlyings = read(*engine, "/api/status")["underlyings"];
  ASSERT_EQ(underlyings.size(), 2);
  EXPECT_EQ(underlyings[1]["symbol"], "XSP");
  EXPECT_EQ(underlyings[1]["paper"]["accepting"], true);
  expect_gate("stalled-spx", "FEED_STALLED");
  const auto accepted = write(*engine, "POST", "/api/orders", order(fresh, "fresh-xsp"));
  EXPECT_EQ(accepted.status, 201) << accepted.body;
}

TEST_F(PaperFeed, StallThresholdUsesProviderDelayAndActiveQuoteAge) {
  seed();
  wall_now = market.time + 16 * md::kNanosPerMinute;
  EXPECT_EQ(paper_status()["accepting"], true);  // exactly delay + max_quote_age
  EXPECT_EQ(write(*engine, "POST", "/api/orders", order(market, "boundary")).status, 201);
  wall_now.fetch_add(1);
  expect_gate("over-boundary", "FEED_STALLED");
  auto risk = read(*engine, "/api/risk");
  risk["limits"]["max_quote_age_seconds"] = 120;
  ASSERT_EQ(write(*engine, "PUT", "/api/risk/limits",
      {{"expected_revision", risk["limits_revision"]}, {"limits", risk["limits"]}}).status, 200);
  EXPECT_EQ(paper_status()["accepting"], true);
  EXPECT_EQ(write(*engine, "POST", "/api/orders", order(market, "larger-age")).status, 201);
}

TEST_F(PaperFeed, StalledRestingOrderWaitsForFreshQuotesWithoutCancellation) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "resting")).status, 201);
  wall_now = market.time + 17 * md::kNanosPerMinute;
  market.next();
  quote("3.80", "4.00");  // a newly received but stale crossing quote
  expect_gate("stalled", "FEED_STALLED");
  const auto resting = read(*engine, "/api/orders?status=open")["orders"];
  ASSERT_EQ(resting.size(), 1);
  EXPECT_EQ(resting[0]["status"], "working");
  EXPECT_TRUE(read(*engine, "/api/fills")["fills"].empty());
  EXPECT_EQ(engine->trading_view()->snapshot->time, market.time);
  market.time = wall_now.load() - 15 * md::kNanosPerMinute;
  quote("3.80", "4.00");
  ASSERT_TRUE(wait_for([&] { return !engine->trading_view()->snapshot->recent_fills.empty(); }));
  EXPECT_EQ(paper_status()["accepting"], true);
  EXPECT_EQ(read(*engine, "/api/fills")["fills"][0]["order_id"], "1");
}

TEST_F(PaperEngine, RestingLimitFillsOnlyOnLaterObservationAndPublishesContractJson) {
  seed();
  const auto response = write(*engine, "POST", "/api/orders", order(market));
  ASSERT_EQ(response.status, 201) << response.body;
  const auto accepted = json::parse(response.body);
  EXPECT_EQ(accepted["order"]["status"], "working");
  EXPECT_TRUE(accepted["fills"].empty());
  const auto immutable = engine->trading_view();
  EXPECT_EQ(read(*engine, "/api/orders?status=open")["orders"].size(), 1);
  market.next(); quote("3.80", "4.00");
  ASSERT_TRUE(wait_for([&] { return !engine->trading_view()->snapshot->recent_fills.empty(); }));
  const auto portfolio = read(*engine, "/api/portfolio");
  EXPECT_EQ(portfolio["cash"], "99599.35");
  EXPECT_EQ(portfolio["fees"], "0.65");
  EXPECT_EQ(portfolio["equity"], "99989.35");
  EXPECT_EQ(portfolio["positions"][0]["average_price"], "4.00");
  EXPECT_EQ(portfolio["positions"][0]["mark"], "3.90");
  EXPECT_TRUE(portfolio["positions"][0]["greeks"]["delta"].is_number());
  EXPECT_TRUE(immutable->snapshot->positions.empty());
  const auto fills = read(*engine, "/api/fills");
  EXPECT_EQ(fills["fills"][0]["price"], "4.00");
  EXPECT_EQ(fills["fills"][0]["id"], "1");
  EXPECT_EQ(fills["fills"][0]["quote_time"], md::format_timestamp(market.time));
  EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["status"], "filled");
  const auto status = read(*engine, "/api/status")["trading"];
  EXPECT_EQ(status["account_version"], portfolio["account_version"]);
  EXPECT_EQ(status["write"], "open");
  EXPECT_EQ(status["fee_per_contract"], "0.65");
  EXPECT_EQ(status["initial_cash"], "100000.00");
  EXPECT_EQ(json::parse(server::tick_message(*engine))["trading"], status);
  const auto chain = read(*engine, "/api/underlyings/SPX/chain");
  EXPECT_EQ(chain["strikes"][0]["call"]["symbol"], market.symbol());
  EXPECT_EQ(chain["strikes"][0]["call"]["bid_size"], 10);
  EXPECT_EQ(chain["strikes"][0]["call"]["tradable"], true);
  EXPECT_TRUE(chain["strikes"][0]["call"]["untradable_reason"].is_null());
}

TEST(PaperStatus, PublishesConfiguredMoneyStringsInStatusAndTick) {
  PaperProvider provider;
  auto options = paper_options();
  options.paper.fee_per_contract = Money::parse("1.234567");
  options.paper.initial_cash = Money::parse("234567.89");
  server::Engine engine(provider, md::Subscription{{"SPX"}}, options);
  const auto configured = read(engine, "/api/status")["trading"];
  EXPECT_EQ(configured["fee_per_contract"], "1.234567");
  EXPECT_EQ(configured["initial_cash"], "234567.89");
  engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  const auto status = read(engine, "/api/status")["trading"];
  EXPECT_EQ(status["fee_per_contract"], "1.234567");
  EXPECT_EQ(status["initial_cash"], "234567.89");
  EXPECT_EQ(json::parse(server::tick_message(engine))["trading"], status);
}

TEST_F(PaperEngine, CancelFillOrderingKillAndRevisionChecks) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "cancel-first")).status, 201);
  auto cancelled = write(*engine, "DELETE", "/api/orders/1");
  ASSERT_EQ(cancelled.status, 200) << cancelled.body;
  EXPECT_EQ(json::parse(cancelled.body)["order"]["reason"]["code"], "USER_CANCEL");
  market.next(); quote("3.80", "4.00");
  EXPECT_TRUE(read(*engine, "/api/fills")["fills"].empty());
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "fill-first")).status, 201);
  expect_error(write(*engine, "DELETE", "/api/orders/2"), 409, "ORDER_TERMINAL");
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "kill", "3.80")).status, 201);
  auto killed = write(*engine, "POST", "/api/risk/kill", {{"action", "trip"}, {"reason", "operator test"}});
  ASSERT_EQ(killed.status, 200) << killed.body;
  EXPECT_EQ(json::parse(killed.body)["cancelled_orders"], json::array({"3"}));
  EXPECT_EQ(json::parse(killed.body)["kill"]["reason"], "operator test");
  expect_error(write(*engine, "POST", "/api/orders", order(market, "blocked")), 422, "KILL_SWITCH");
  auto reset = write(*engine, "POST", "/api/risk/kill", {{"action", "reset"}, {"reason", "reviewed"}});
  EXPECT_EQ(reset.status, 200);
  EXPECT_FALSE(json::parse(reset.body)["kill"]["latched"].get<bool>());
  auto risk = read(*engine, "/api/risk");
  EXPECT_EQ(risk["scenarios"]["pnl"].size(), 9);
  EXPECT_EQ(risk["scenarios"]["pnl"][0].size(), 4);
  EXPECT_EQ(risk["scenarios"]["pnl"][4][1], 0);
  auto limits = risk["limits"];
  limits["max_order_contracts"] = 2;
  const auto updated = write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "1"}, {"limits", limits}});
  EXPECT_EQ(updated.status, 200) << updated.body;
  EXPECT_EQ(json::parse(updated.body)["limits_revision"], "2");
  expect_error(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "1"}, {"limits", limits}}), 409, "LIMITS_REVISION");
  limits["max_order_contracts"] = 0;
  expect_error(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "2"}, {"limits", limits}}), 422, "INVALID_LIMITS");
}

TEST_F(PaperEngine, ErrorsRejectMalformedUnknownFieldsAndRecordBusinessRejections) {
  seed();
  auto request = order(market);
  request["unexpected"] = true;
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request = order(market); request["quantity"] = 1.5;
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request["quantity"] = "1";
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request = order(market); request["quantity"] = std::numeric_limits<std::uint64_t>::max();
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request = order(market); request["limit_price"] = 4.0;
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request = order(market); request["limit_price"] = "4e0";
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  for (const auto* body : {"{", "[]", "{\"action\":\"trip\",\"action\":\"reset\",\"reason\":\"x\"}"}) {
    server::ApiResponse response;
    server::ApiRequest raw{"POST", "/api/risk/kill", body};
    server::handle_api_async(raw, *engine, [&](auto r) { response = std::move(r); });
    expect_error(response, 400, "INVALID_REQUEST");
  }
  expect_error(write(*engine, "DELETE", "/api/orders/bad"), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "DELETE", "/api/orders/999"), 404, "UNKNOWN_ORDER");
  request = order(market); request["symbol"] = "SPXW  261022C05100000";
  expect_error(write(*engine, "POST", "/api/orders", request), 404, "UNKNOWN_CONTRACT");
  request = order(market, "bad-tick", "4.01");
  expect_error(write(*engine, "POST", "/api/orders", request), 422, "INVALID_TICK");
  EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["status"], "rejected");
  expect_error(write(*engine, "POST", "/api/orders", request), 409, "DUPLICATE_CLIENT_ID");
  expect_error(write(*engine, "POST", "/api/settlements", {{"symbol", market.symbol()}, {"value", "5000.00"}}), 422, "INVALID_SETTLEMENT");
  expect_error(server::handle_api({"GET", "/api/orders?status=closed"}, *engine), 400, "INVALID_REQUEST");
  expect_error(server::handle_api({"GET", "/api/missing"}, *engine), 404, "NOT_FOUND");
}

TEST_F(PaperEngine, FractionalSizesNeverRoundUpAndCachedObservationsDoNotRefill) {
  seed("4.00", "4.20", 1.9);
  auto first = write(*engine, "POST", "/api/orders", order(market, "first", "4.20"));
  ASSERT_EQ(first.status, 201) << first.body;
  EXPECT_EQ(json::parse(first.body)["order"]["status"], "filled");
  auto second = write(*engine, "POST", "/api/orders", order(market, "second", "4.20"));
  ASSERT_EQ(second.status, 201) << second.body;
  EXPECT_EQ(json::parse(second.body)["order"]["status"], "working");
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time + 1, 5000, 5000, 5000});
  EXPECT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->time == market.time + 1; }));
  EXPECT_EQ(engine->trading_view()->snapshot->recent_fills.size(), 1);
  market.next(); quote("4.00", "4.20", 1.9);
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->recent_fills.size() == 2; }));
}

TEST_F(PaperEngine, EligibilityUsesTheDefinitionAndRecordsRejection) {
  // An American contract on a European index root is not listed.
  market.contract.style = pricing::ExerciseStyle::American;
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  ASSERT_TRUE(wait_for([&] { return engine->status().contracts == 1; }));
  expect_error(write(*engine, "POST", "/api/orders", order(market)), 422, "AMERICAN_UNSUPPORTED");
  EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["status"], "rejected");
}

TEST(PaperEquity, SpyOptionsAreTradableAndFillAgainstTheirOwnBook) {
  PaperProvider provider;
  server::Engine engine(provider, md::Subscription{{"SPY"}}, paper_options());
  engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  test::ScriptedMarket market;
  market.contract = *md::parse_osi("SPY261022C00500000");
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPY", market.time, 500, 500, 500});
  provider.sink->publish(md::OptionQuote{0, market.time, 4.00, 4.20, 10, 10});
  ASSERT_TRUE(wait_for([&] {
    const auto metrics = engine.metrics("SPY");
    return metrics && metrics->as_of == market.time && !metrics->slices.empty();
  }));
  EXPECT_TRUE(read(engine, "/api/status")["underlyings"][0]["has_tradable_contracts"].get<bool>());
  const auto response = write(engine, "POST", "/api/orders", order(market, "spy", "4.20"));
  ASSERT_EQ(response.status, 201) << response.body;
  const auto body = json::parse(response.body);
  EXPECT_EQ(body["order"]["status"], "filled");
  EXPECT_EQ(body["order"]["day_end"], md::format_timestamp(md::new_york_to_utc({2026, 9, 22}, 16, 15)));
  EXPECT_EQ(read(engine, "/api/portfolio")["positions"][0]["symbol"], market.symbol());
  engine.stop();
}

TEST_F(PaperEngine, PmUsesFirstExpiryDatePrintAndAmWaitsForImport) {
  market.contract = *md::parse_osi("SPXW260922C05000000");
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "pm", "4.20")).status, 201);
  auto expiry = market.contract.expiry_time();
  provider.sink->publish(md::UnderlyingQuote{"SPX", expiry - 1, 5010, 5010, 5010});
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->time == expiry - 1; }));
  EXPECT_EQ(read(*engine, "/api/portfolio")["positions"].size(), 1);
  provider.sink->publish(md::UnderlyingQuote{"SPX", expiry, 5012, 5012, 5012});
  provider.sink->publish(md::UnderlyingQuote{"SPX", expiry + 1, 5020, 5020, 5020});
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->positions.empty(); }));
  EXPECT_EQ(read(*engine, "/api/portfolio")["cash"], "100779.35");

  market.contract = *md::parse_osi("SPX260923C05000000");
  market.time = md::new_york_to_utc({2026, 9, 22}, 16, 5);
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "am", "4.20")).status, 201);
  expiry = market.contract.expiry_time();
  provider.sink->publish(md::UnderlyingQuote{"SPX", expiry, 5050, 5050, 5050});
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->time == expiry; }));
  const auto waiting = read(*engine, "/api/portfolio");
  ASSERT_EQ(waiting["positions"].size(), 1);
  EXPECT_EQ(waiting["positions"][0]["awaiting_settlement"], true);
  EXPECT_EQ(waiting["valuation_complete"], false);
  EXPECT_TRUE(waiting["positions"][0]["greeks"]["delta"].is_null());
  auto settlement = write(*engine, "POST", "/api/settlements", {{"symbol", market.symbol()}, {"value", "5010.00"}});
  ASSERT_EQ(settlement.status, 200) << settlement.body;
  EXPECT_EQ(json::parse(settlement.body)["position_closed"], true);
  expect_error(write(*engine, "POST", "/api/settlements", {{"symbol", market.symbol()}, {"value", "5010.00"}}), 422, "ALREADY_SETTLED");
}

TEST(PaperRecovery, RestartRestoresIdenticalPortfolioRiskAndLiquidityBudget) {
  const auto path = std::filesystem::temp_directory_path() / ("openport-paper-" + std::to_string(md::now()) + ".jsonl");
  auto options = paper_options(); options.paper_journal = path;
  test::ScriptedMarket market;
  std::atomic<md::Timestamp> wall_now{market.time};
  options.clock = [&] { return wall_now.load(); };
  std::string portfolio, risk;
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options); engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    provider.sink->publish(md::ContractDefinition{0, market.contract});
    provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
    ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == market.time; }));
    auto response = write(engine, "POST", "/api/orders", order(market, "buy", "4.20"));
    ASSERT_EQ(response.status, 201) << response.body;
    engine.stop();
    portfolio = server::handle_api({"GET", "/api/portfolio"}, engine).body;
    risk = server::handle_api({"GET", "/api/risk"}, engine).body;
  }
  // A resumed journal owns its original cash and fee schedule, not new options.
  options.paper.fee_per_contract = Money::parse("9.00");
  options.paper.initial_cash = Money::parse("500.00");
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options); engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    EXPECT_EQ(server::handle_api({"GET", "/api/portfolio"}, engine).body, portfolio);
    EXPECT_EQ(server::handle_api({"GET", "/api/risk"}, engine).body, risk);
    const auto status = read(engine, "/api/status")["trading"];
    EXPECT_EQ(status["fee_per_contract"], "0.65");
    EXPECT_EQ(status["initial_cash"], "100000.00");
    EXPECT_EQ(json::parse(server::tick_message(engine))["trading"], status);
    auto response = write(engine, "POST", "/api/orders", order(market, "rest", "4.20"));
    ASSERT_EQ(response.status, 201) << response.body;
    EXPECT_EQ(json::parse(response.body)["order"]["status"], "working");
    wall_now = market.time + 20 * md::kNanosPerMinute;
    const auto paper = read(engine, "/api/status")["underlyings"][0]["paper"];
    EXPECT_EQ(paper["accepting"], false);
    EXPECT_EQ(paper["reason"], "FEED_STALLED");
    EXPECT_EQ(json::parse(server::tick_message(engine))["underlyings"][0]["paper"], paper);
    const auto stalled = write(engine, "POST", "/api/orders", order(market, "recovered-stall", "4.20"));
    ASSERT_EQ(stalled.status, 422) << stalled.body;
    EXPECT_EQ(json::parse(stalled.body)["error"]["code"], "FEED_STALLED");
    EXPECT_EQ(json::parse(stalled.body)["error"]["message"], paper["message"]);
    EXPECT_EQ(read(engine, "/api/orders?status=open")["orders"].size(), 1);
    EXPECT_EQ(read(engine, "/api/fills")["fills"].size(), 1);
    provider.sink->publish(md::ContractDefinition{0, market.contract});
    market.time = wall_now.load();
    market.next();
    provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
    provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->recent_fills.size() == 2; }));
  }
  const auto recovered = trading::TradingSession::recover(trading::FileJournal::read(path.string()));
  EXPECT_EQ(recovered.snapshot()->recent_orders.back().reason.code, trading::Reason::FEED_STALLED);
  std::filesystem::remove(path);
}

TEST(PaperAvailability, DisabledFailedJournalFullInboxAndStoppingFailClosed) {
  for (int mode = 0; mode < 4; ++mode) {
    PaperProvider provider;
    auto options = paper_options();
    if (mode == 0) options.paper_enabled = false;
    if (mode == 1) options.paper_journal = std::filesystem::temp_directory_path();
    if (mode == 2) options.command_capacity = 0;
    server::Engine engine(provider, {{"SPX"}}, options); engine.start();
    if (mode == 1) {
      ASSERT_TRUE(wait_for([&] { return engine.status().trading.reason.starts_with("JOURNAL_IO"); }));
    }
    if (mode >= 2) {
      ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    }
    if (mode == 3) engine.stop();
    const auto response = write(engine, "POST", "/api/risk/kill", {{"action", "trip"}, {"reason", "test"}});
    EXPECT_EQ(response.status, 503) << response.body;
    EXPECT_EQ(json::parse(response.body)["error"]["code"], "TRADING_UNAVAILABLE");
    if (mode == 0) {
      EXPECT_FALSE(engine.status().trading.enabled);
    }
    if (mode == 1) {
      EXPECT_EQ(engine.status().trading.write, "disabled");
    }
  }
}

TEST(PaperAvailability, LockedJournalDisablesEveryWriteButKeepsAnalyticsAndOwnerWorking) {
  const auto path = std::filesystem::temp_directory_path() /
      ("openport-locked-" + std::to_string(md::now()) + ".jsonl");
  auto bytes = [&] {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
  };
  auto options = paper_options();
  options.paper_journal = path;
  PaperProvider first_provider;
  server::Engine first(first_provider, {{"SPX"}}, options);
  first.start();
  ASSERT_NE(first.trading_view(), nullptr);
  const auto limits = read(first, "/api/risk")["limits"];
  const auto before = bytes();
  ASSERT_FALSE(before.empty());
  {
    PaperProvider second_provider;
    server::Engine second(second_provider, {{"SPX"}}, options);
    second.start();
    const auto reason = "JOURNAL_LOCKED: paper journal '" + path.string() +
        "' is in use by another openportd; use --paper-journal to choose another file or --no-paper";
    const auto status = read(second, "/api/status")["trading"];
    EXPECT_FALSE(status["enabled"].get<bool>());
    EXPECT_EQ(status["write"], "disabled");
    EXPECT_EQ(status["reason"], reason);
    EXPECT_EQ(second.trading_view(), nullptr);
    EXPECT_EQ(bytes(), before);

    test::ScriptedMarket market;
    second_provider.sink->publish(md::ContractDefinition{0, market.contract});
    second_provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    second_provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
    ASSERT_TRUE(wait_for([&] {
      const auto metrics = second.metrics("SPX");
      return metrics && metrics->as_of == market.time && !metrics->slices.empty();
    }));
    struct Request { std::string method; std::string path; json body; };
    const std::vector<Request> requests{
        {"POST", "/api/orders", order(market)},
        {"DELETE", "/api/orders/1", nullptr},
        {"PUT", "/api/risk/limits", {{"expected_revision", "1"}, {"limits", limits}}},
        {"POST", "/api/risk/kill", {{"action", "trip"}, {"reason", "test"}}},
        {"POST", "/api/risk/kill", {{"action", "reset"}, {"reason", "test"}}},
        {"POST", "/api/settlements", {{"symbol", market.symbol()}, {"value", "5000.00"}}},
        // Unavailability also takes precedence over malformed command bodies.
        {"POST", "/api/orders", json::object()}};
    for (const auto& request : requests) {
      const auto response = write(second, request.method, request.path, request.body);
      EXPECT_EQ(response.status, 503) << response.body;
      EXPECT_EQ(json::parse(response.body)["error"]["code"], "TRADING_UNAVAILABLE");
      EXPECT_EQ(json::parse(response.body)["error"]["message"], reason);
    }
    EXPECT_EQ(bytes(), before);
    ASSERT_EQ(write(first, "POST", "/api/risk/kill", {{"action", "trip"}, {"reason", "owner"}}).status, 200);
    EXPECT_TRUE(first.status().trading.enabled);
    EXPECT_TRUE(first.status().trading.reason.empty());
    const auto after_owner_write = bytes();
    EXPECT_NE(after_owner_write, before);
    second.stop();
    EXPECT_EQ(bytes(), after_owner_write);
    // Neither the rejected opens nor the second engine's shutdown released the owner's lock.
    EXPECT_THROW(trading::FileJournal::resume(path.string()), trading::TradingError);
  }
  first.stop();
  const auto recovery = trading::FileJournal::read(path.string());
  EXPECT_FALSE(recovery.truncated_final_line);
  const auto recovered = trading::TradingSession::recover(recovery).snapshot();
  EXPECT_TRUE(recovered->risk.kill_latched);
  EXPECT_EQ(recovered->risk.kill_reason, "owner");
  EXPECT_EQ(recovered->account_version, first.trading_view()->snapshot->account_version);
  std::filesystem::remove(path);
}

TEST(PaperWritePolicy, ProtectsEveryWriteAndLeavesReadsOpen) {
  for (const std::string method : {"POST", "PUT", "DELETE"}) {
    server::ApiRequest request{method, "/api/anything"};
    request.content_type = "application/json"; request.host = "localhost:8080";
    for (const std::string address : {"127.0.0.1", "127.0.0.2", "::1"})
      EXPECT_FALSE(server::check_api_write(request, {address, "", {}}));
    auto error = server::check_api_write(request, {"0.0.0.0", "", {}});
    ASSERT_TRUE(error); EXPECT_EQ(error->status, 403);
    EXPECT_EQ(json::parse(error->body)["error"]["code"], "WRITE_DISABLED");
    for (const std::string address : {"127.0.0.1", "0.0.0.0", "::"}) {
      request.authorization = "Bearer wrong";
      error = server::check_api_write(request, {address, "secret", {}});
      ASSERT_TRUE(error);
      EXPECT_EQ(json::parse(error->body)["error"]["code"], "WRITE_TOKEN_REQUIRED");
      request.authorization = "Bearer secret";
      EXPECT_FALSE(server::check_api_write(request, {address, "secret", {}}));
    }
    request.origin = "https://evil.test";
    error = server::check_api_write(request, {});
    ASSERT_TRUE(error); EXPECT_EQ(json::parse(error->body)["error"]["code"], "ORIGIN_REJECTED");
    EXPECT_FALSE(server::check_api_write(request, {"127.0.0.1", "", {"https://evil.test"}}));
    request.origin = "http://localhost:8080";
    EXPECT_FALSE(server::check_api_write(request, {}));
    request.content_type = "text/plain";
    if (method != "DELETE") {
      error = server::check_api_write(request, {});
      ASSERT_TRUE(error); EXPECT_EQ(error->status, 400);
    }
    request.method = "GET";
    EXPECT_FALSE(server::check_api_write(request, {"0.0.0.0", "secret", {}}));
  }
}
}  // namespace

namespace {
TEST_F(PaperEngine, DailyLossTripsBeforeCrossingOrdersAndRolloverWaitsForMarks) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "position", "4.20")).status, 201);
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "rest", "4.00")).status, 201);
  auto limits = read(*engine, "/api/risk")["limits"];
  limits["max_daily_loss"] = "20.00";
  ASSERT_EQ(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "1"}, {"limits", limits}}).status, 200);
  market.next(); quote("3.80", "4.00");
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->risk.kill_latched; }));
  EXPECT_EQ(engine->trading_view()->snapshot->recent_fills.size(), 1);
  EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["status"], "cancelled");
  market.time += md::kNanosPerDay;
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->time == market.time; }));
  EXPECT_EQ(read(*engine, "/api/portfolio")["start_of_day_equity"], "100000.00");
  quote("3.80", "4.00");
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->valuation_complete; }));
  const auto portfolio = read(*engine, "/api/portfolio");
  EXPECT_EQ(portfolio["start_of_day_equity"], portfolio["equity"]);
  EXPECT_EQ(portfolio["day_pnl"], "0.00");
  EXPECT_TRUE(engine->status().trading.kill_latched);
}

TEST(PaperOrdering, MarketBatchPrecedesCancelAndInboxIsBoundedWhileOwnerIsBusy) {
  struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool armed = false, paused = false, released = false;
  } gate;
  auto options = paper_options(); options.command_capacity = 2;
  options.monotonic_clock = [&] {
    std::unique_lock lock(gate.mutex);
    if (gate.armed) {
      gate.armed = false; gate.paused = true; gate.changed.notify_all();
      gate.changed.wait(lock, [&] { return gate.released; });
    }
    return std::chrono::steady_clock::now();
  };
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  struct ReleaseGate {
    Gate& gate;
    ~ReleaseGate() {
      const std::lock_guard lock(gate.mutex);
      gate.released = true;
      gate.changed.notify_all();
    }
  } release{gate};
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  test::ScriptedMarket market;
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
  provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
  ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == market.time; }));
  ASSERT_EQ(write(engine, "POST", "/api/orders", order(market)).status, 201);
  {
    std::unique_lock lock(gate.mutex);
    gate.armed = true;
    ASSERT_TRUE(gate.changed.wait_for(lock, std::chrono::seconds(3), [&] { return gate.paused; }));
  }
  market.next();
  provider.sink->publish(md::OptionQuote{0, market.time, 3.8, 4, 1, 1});
  server::TradingCommand cancel; cancel.kind = server::TradingCommand::Kind::Cancel; cancel.order_id = 1;
  std::promise<server::TradingReply> first, second;
  const bool accepted_first = engine.post_trading(cancel, [&](auto reply) { first.set_value(std::move(reply)); });
  const bool accepted_second = engine.post_trading(cancel, [&](auto reply) { second.set_value(std::move(reply)); });
  const bool full = engine.post_trading(cancel, [](auto) {});
  {
    const std::lock_guard lock(gate.mutex);
    gate.released = true; gate.changed.notify_all();
  }
  ASSERT_TRUE(accepted_first); ASSERT_TRUE(accepted_second); EXPECT_FALSE(full);
  const auto a = first.get_future().get(), b = second.get_future().get();
  EXPECT_EQ(a.decision.code, trading::Reason::ORDER_TERMINAL);
  EXPECT_EQ(a.view->snapshot->recent_fills.size(), 1);
  EXPECT_EQ(b.view->snapshot->account_version, a.view->snapshot->account_version + 1);
}

class FailingPaperJournal final : public trading::Journal {
 public:
  void append(md::Timestamp, std::string_view, std::string_view) override {
    if (failed) throw trading::TradingError(trading::Reason::JOURNAL_IO, "injected disk failure");
    ++sequence_;
  }
  std::uint64_t sequence() const override { return sequence_; }
  std::string head() const override { return std::string(64, '0'); }
  std::atomic<bool> failed{false};
 private:
  std::uint64_t sequence_ = 0;
};
TEST(PaperAvailability, RuntimeJournalFailurePreservesAccountAndDisablesWrites) {
  auto journal = std::make_shared<FailingPaperJournal>();
  auto options = paper_options(); options.paper_sink = journal;
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, options); engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  const auto before = engine.trading_view()->snapshot;
  journal->failed = true;
  auto response = write(engine, "POST", "/api/risk/kill", {{"action", "trip"}, {"reason", "test"}});
  EXPECT_EQ(response.status, 503);
  EXPECT_EQ(json::parse(response.body)["error"]["code"], "TRADING_UNAVAILABLE");
  const auto after = engine.trading_view()->snapshot;
  EXPECT_EQ(after->account.cash, before->account.cash);
  EXPECT_EQ(after->account_version, before->account_version);
  EXPECT_TRUE(after->journal_failed);
  EXPECT_EQ(engine.status().trading.write, "disabled");
  EXPECT_TRUE(engine.status().trading.reason.starts_with("JOURNAL_IO"));
  EXPECT_EQ(write(engine, "POST", "/api/risk/kill", {{"action", "reset"}, {"reason", "test"}}).status, 503);
}
}  // namespace

namespace {
TEST(PaperQueue, PreservesClosingPrintsWhileOptionQuotesCoalesce) {
  md::EventQueue queue(10, true);
  const test::ScriptedMarket market;
  const auto expiry = market.contract.expiry_time();
  queue.publish(md::UnderlyingQuote{"SPX", expiry - 1, 0, 0, 4999});
  queue.publish(md::UnderlyingQuote{"SPX", expiry, 0, 0, 5001});
  queue.publish(md::UnderlyingQuote{"SPX", expiry + 1, 0, 0, 5010});
  queue.publish(md::OptionQuote{0, expiry - 2, 3, 3.1, 1, 1});
  queue.publish(md::OptionQuote{0, expiry - 1, 4, 4.1, 1, 1});
  std::vector<md::Event> events;
  EXPECT_EQ(queue.drain(events, std::chrono::milliseconds(0)), 4);
  EXPECT_EQ(std::get<md::UnderlyingQuote>(events[1]).last, 5001);
  EXPECT_EQ(std::get<md::OptionQuote>(events[3]).bid, 4);
  EXPECT_EQ(queue.status().coalesced, 1);
}

TEST_F(PaperEngine, ValidatesConditionalOrderFieldsAndReturnsNumericRiskEvidence) {
  seed();
  auto oversized = order(market, "oversized"); oversized["quantity"] = 101;
  const auto size_rejection = write(*engine, "POST", "/api/orders", oversized);
  expect_error(size_rejection, 422, "MAX_ORDER_CONTRACTS");
  EXPECT_EQ(json::parse(size_rejection.body)["error"]["scope"], "SPX");
  auto request = order(market);
  request.erase("limit_price");
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request = order(market); request["type"] = "market";
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request.erase("limit_price"); request["time_in_force"] = "day";
  expect_error(write(*engine, "POST", "/api/orders", request), 422, "INVALID_ORDER");
  request["client_order_id"] = "market"; request["time_in_force"] = "ioc";
  const auto market_order = write(*engine, "POST", "/api/orders", request);
  ASSERT_EQ(market_order.status, 201) << market_order.body;
  EXPECT_EQ(json::parse(market_order.body)["order"]["limit_price"], nullptr);
  EXPECT_EQ(json::parse(market_order.body)["order"]["status"], "filled");
  auto limits = read(*engine, "/api/risk")["limits"];
  limits["aggregate"]["dollar_delta"] = 0;
  limits["per_underlying"]["dollar_delta"] = 0;
  ASSERT_EQ(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "1"}, {"limits", limits}}).status, 200);
  const auto rejection = write(*engine, "POST", "/api/orders", order(market, "risk"));
  expect_error(rejection, 422, "DELTA_LIMIT");
  const auto error = json::parse(rejection.body)["error"];
  EXPECT_GT(error["actual"].get<double>(), 0);
  EXPECT_EQ(error["limit"], 0);
  EXPECT_EQ(error["scope"], "SPX");
  limits["aggregate"]["unexpected"] = 1;
  expect_error(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "2"}, {"limits", limits}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/risk/kill", {{"action", "reset"}, {"reason", " "}}), 422, "INVALID_REASON");
  expect_error(write(*engine, "POST", "/api/risk/kill", {{"action", "bad"}, {"reason", "test"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/settlements", {{"symbol", market.symbol()}, {"value", 5000}}), 400, "INVALID_REQUEST");
}
}  // namespace

namespace {
TEST(PaperRecovery, SettlementProvenanceIsDurableAndStopReleasesJournalWriter) {
  const auto path = std::filesystem::temp_directory_path() / ("openport-settlement-" + std::to_string(md::now()) + ".jsonl");
  auto options = paper_options(); options.paper_journal = path;
  test::ScriptedMarket market;
  market.contract = *md::parse_osi("SPXW260922C05000000");
  PaperProvider provider;
  server::Engine original(provider, {{"SPX"}}, options); original.start();
  ASSERT_TRUE(wait_for([&] { return original.trading_view() != nullptr; }));
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
  provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
  ASSERT_TRUE(wait_for([&] { return original.metrics("SPX") && original.metrics("SPX")->as_of == market.time; }));
  ASSERT_EQ(write(original, "POST", "/api/orders", order(market, "pm", "4.20")).status, 201);
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.contract.expiry_time(), 0, 0, 5012});
  ASSERT_TRUE(wait_for([&] { return original.trading_view()->snapshot->positions.empty(); }));
  original.stop();
  const auto recovery = trading::FileJournal::read(path.string());
  bool found = false;
  for (const auto& record : recovery.records) {
    if (record.type != "settlement") continue;
    const auto source = json::parse(record.payload)["settlement_source"];
    EXPECT_EQ(source["kind"], "provider_closing_print");
    EXPECT_EQ(source["provider"], "scripted paper");
    EXPECT_EQ(source["quote_time"], md::format_timestamp(market.contract.expiry_time()));
    found = true;
  }
  EXPECT_TRUE(found);
  PaperProvider replacement_provider;
  server::Engine replacement(replacement_provider, {{"SPX"}}, options); replacement.start();
  ASSERT_TRUE(wait_for([&] { return replacement.trading_view() != nullptr; }));
  EXPECT_EQ(server::handle_api({"GET", "/api/portfolio"}, replacement).body,
            server::handle_api({"GET", "/api/portfolio"}, original).body);
  replacement.stop();
  std::filesystem::remove(path);
}

TEST(PaperPlans, PresetsListExactRules) {
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, paper_options());
  const auto plans = read(engine, "/api/plans")["plans"];
  ASSERT_EQ(plans.size(), 7);
  EXPECT_EQ(plans[0]["id"], "practice");
  EXPECT_EQ(plans[0]["rules"]["profit_target"], nullptr);
  EXPECT_EQ(plans[0]["rules"]["buying_power"], true);
  const auto intraday = plans[3];
  EXPECT_EQ(intraday["id"], "intraday-100k");
  EXPECT_EQ(intraday["name"], "Intraday 100K");
  EXPECT_EQ(intraday["initial_cash"], "100000.00");
  EXPECT_EQ(intraday["rules"], json({{"plan", "Intraday 100K"}, {"profit_target", "10000.00"}, {"max_drawdown", "5000.00"},
      {"drawdown_mode", "intraday"}, {"buy_only", true}, {"buying_power", true}, {"expiry_cutoff_seconds", 300}}));
  const auto eod = plans[4];
  EXPECT_EQ(eod["id"], "eod-25k");
  EXPECT_EQ(eod["rules"]["profit_target"], "3000.00");
  EXPECT_EQ(eod["rules"]["max_drawdown"], "1500.00");
  EXPECT_EQ(eod["rules"]["drawdown_mode"], "end_of_day");
  EXPECT_EQ(eod["rules"]["buy_only"], false);
}

TEST_F(PaperEngine, AccountViewWithoutRulesHasNoTargetOrFloor) {
  const auto account = read(*engine, "/api/account");
  EXPECT_EQ(account["rules"]["plan"], nullptr);
  EXPECT_EQ(account["evaluation"]["enabled"], false);
  EXPECT_EQ(account["evaluation"]["status"], "active");
  EXPECT_EQ(account["evaluation"]["attempt"], 1);
  EXPECT_EQ(account["evaluation"]["starting_balance"], "100000.00");
  EXPECT_EQ(account["evaluation"]["floor"], nullptr);
  EXPECT_EQ(account["evaluation"]["target_equity"], nullptr);
  EXPECT_EQ(account["buying_power"]["available"], "100000.00");
  EXPECT_TRUE(account["attempts"].empty());
  const auto trading = read(*engine, "/api/status")["trading"];
  EXPECT_EQ(trading["plan"], nullptr);
  EXPECT_EQ(trading["evaluation"], nullptr);
  engine->stop();
}

TEST_F(PaperEngine, ResetToPresetStartsAttemptAndTradesSeparateAttempts) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "open", "4.20")).status, 201);
  auto trades = read(*engine, "/api/trades")["trades"];
  ASSERT_EQ(trades.size(), 1);
  EXPECT_EQ(trades[0]["status"], "open");
  EXPECT_EQ(trades[0]["direction"], "long");
  EXPECT_EQ(trades[0]["average_open"], "4.20");
  EXPECT_EQ(trades[0]["mark"], "4.10");
  EXPECT_EQ(trades[0]["unrealised"], "-10.00");
  EXPECT_EQ(trades[0]["attempt"], 1);
  EXPECT_EQ(read(*engine, "/api/orders?status=all")["orders"][0]["origin"], "user");

  const auto reset = write(*engine, "POST", "/api/account/reset", {{"plan", "intraday-25k"}, {"reason", "start evaluation"}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  const auto account = json::parse(reset.body);
  EXPECT_EQ(account["rules"]["plan"], "Intraday 25K");
  const auto evaluation = account["evaluation"];
  EXPECT_EQ(evaluation["enabled"], true);
  EXPECT_EQ(evaluation["attempt"], 2);
  EXPECT_EQ(evaluation["starting_balance"], "25000.00");
  EXPECT_EQ(evaluation["equity"], "25000.00");
  EXPECT_EQ(evaluation["floor"], "23750.00");
  EXPECT_EQ(evaluation["drawdown_buffer"], "1250.00");
  EXPECT_EQ(evaluation["target_equity"], "27500.00");
  EXPECT_EQ(evaluation["target_remaining"], "2500.00");
  EXPECT_EQ(account["attempts"][0]["final_equity"], "99989.35");
  EXPECT_EQ(read(*engine, "/api/account"), account);

  EXPECT_TRUE(read(*engine, "/api/trades")["trades"].empty());
  trades = read(*engine, "/api/trades?status=closed&attempt=all")["trades"];
  ASSERT_EQ(trades.size(), 1);
  EXPECT_EQ(trades[0]["closure"], "reset");
  EXPECT_EQ(trades[0]["attempt"], 1);
  EXPECT_EQ(trades[0]["gross"], "-10.00");
  EXPECT_EQ(trades[0]["fees"], "0.65");
  EXPECT_EQ(trades[0]["net"], "-10.65");
  EXPECT_EQ(trades[0]["cost"], "420.00");
  EXPECT_NEAR(trades[0]["return"].get<double>(), -10.65 / 420, 1e-12);
  const auto trading = read(*engine, "/api/status")["trading"];
  EXPECT_EQ(trading["plan"], "Intraday 25K");
  EXPECT_EQ(trading["evaluation"], "active");
  EXPECT_EQ(json::parse(server::tick_message(*engine))["trading"], trading);

  auto sell = order(market, "naked-sell", "4.00");
  sell["side"] = "sell";
  expect_error(write(*engine, "POST", "/api/orders", sell), 422, "BUY_ONLY");
  engine->stop();
}

TEST_F(PaperEngine, ResetRequestsAreStrict) {
  expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "platinum"}, {"reason", "x"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"initial_cash", "1"}, {"reason", "x"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/account/reset", {{"reason", "x"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "  "}}), 422, "INVALID_REASON");
  const json rules{{"plan", "Custom"}, {"profit_target", nullptr}, {"max_drawdown", "10.00"}, {"drawdown_mode", "sideways"},
                   {"buy_only", false}, {"buying_power", false}, {"expiry_cutoff_seconds", 0}};
  expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", rules}, {"reason", "x"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "-5"}, {"rules", rules}, {"reason", "x"}}), 400, "INVALID_REQUEST");
  const auto bad_query = server::handle_api({"GET", "/api/trades?status=open&status=all"}, *engine);
  EXPECT_EQ(bad_query.status, 400);
  EXPECT_EQ(server::handle_api({"GET", "/api/trades?attempt=previous"}, *engine).status, 400);
  engine->stop();
}

TEST_F(PaperEngine, CustomDrawdownBreachLiquidatesWithSystemOrders) {
  seed();
  const json rules{{"plan", "Tight"}, {"profit_target", nullptr}, {"max_drawdown", "10.00"}, {"drawdown_mode", "intraday"},
                   {"buy_only", false}, {"buying_power", false}, {"expiry_cutoff_seconds", 0}};
  ASSERT_EQ(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", rules}, {"reason", "tight"}}).status, 200);
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "breach", "4.20")).status, 201);
  const auto account = read(*engine, "/api/account");
  EXPECT_EQ(account["evaluation"]["status"], "failed");
  EXPECT_EQ(account["evaluation"]["decided_equity"], "9989.35");
  EXPECT_NE(account["evaluation"]["decision"].get<std::string>().find("drawdown floor $9990.00"), std::string::npos);
  const auto orders = read(*engine, "/api/orders?status=all")["orders"];
  EXPECT_EQ(orders[0]["origin"], "system");
  EXPECT_TRUE(orders[0]["client_order_id"].get<std::string>().starts_with("system:drawdown:"));
  EXPECT_EQ(orders[0]["status"], "filled");
  EXPECT_TRUE(read(*engine, "/api/portfolio")["positions"].empty());
  EXPECT_EQ(read(*engine, "/api/portfolio")["cash"], "9978.70");
  EXPECT_EQ(json::parse(server::tick_message(*engine))["trading"]["evaluation"], "failed");
  expect_error(write(*engine, "POST", "/api/orders", order(market, "after", "4.20")), 422, "EVALUATION_CLOSED");
  engine->stop();
}
}  // namespace

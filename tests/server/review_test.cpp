#include <gtest/gtest.h>
#include <set>

#include "openport/server/api.hpp"
#include "server/paper_csv.hpp"
#include "support/scripted_market.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;
class ReviewSource final : public server::MetricsSource {
 public:
  std::shared_ptr<server::TradingView> view;
  std::vector<std::string> symbols() const override { return {}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return {}; }
  server::EngineStatus status() const override { server::EngineStatus s; s.provider = "replay (demo)"; return s; }
  using server::MetricsSource::trading_view;
  std::shared_ptr<const server::TradingView> trading_view() const override { return view; }
  void publish(const trading::TradingSession& session) {
    view = std::make_shared<server::TradingView>();
    view->snapshot = session.snapshot(); view->contracts = session.contracts(); view->config = session.config();
  }
};
/// Parse quoted fields, including embedded newlines, independently of the writer.
std::vector<std::vector<std::string>> csv_rows(const std::string& csv) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool quoted = false;
  for (std::size_t i = 0; i < csv.size(); ++i) {
    const auto c = csv[i];
    if (c == '"') {
      if (quoted && i + 1 < csv.size() && csv[i + 1] == '"') { field += '"'; ++i; }
      else quoted = !quoted;
    } else if (c == ',' && !quoted) { row.push_back(field); field.clear(); }
    else if (c == '\r' && !quoted && i + 1 < csv.size() && csv[i + 1] == '\n') {
      row.push_back(field); rows.push_back(row); row.clear(); field.clear(); ++i;
    } else field += c;
  }
  EXPECT_FALSE(quoted); EXPECT_TRUE(row.empty()); EXPECT_TRUE(field.empty());
  return rows;
}
json csv_record(const std::vector<std::string>& columns, const std::vector<std::string>& row) {
  EXPECT_EQ(columns.size(), row.size());
  json out = json::object();
  for (std::size_t i = 0; i < std::min(columns.size(), row.size()); ++i) out[columns[i]] = row[i];
  return out;
}
void check_columns(const json& value, const std::set<std::string>& columns, const std::string& prefix = {}) {
  for (const auto& [key, field] : value.items()) {
    const auto path = prefix.empty() ? key : prefix + "." + key;
    if (field.is_object()) check_columns(field, columns, path);
    else if (field.is_null()) {
      EXPECT_TRUE(columns.contains(path) || std::any_of(columns.begin(), columns.end(), [&](const auto& c) { return c.starts_with(path + "."); })) << path;
    } else EXPECT_TRUE(columns.contains(path)) << path;
  }
}

TEST(TradeReviewApi, CsvQuotesNotesAndKeepsColumnsAndExactMoneyWithEmptyHistory) {
  test::ScriptedMarket f;
  trading::SessionConfig config; config.fee_per_contract = Money::parse("0.650001");
  trading::TradingSession s(config, f.time); f.seed(s);
  ReviewSource source; source.publish(s);
  const auto empty = server::handle_api({"GET", "/api/trades.csv"}, source);
  ASSERT_EQ(empty.status, 200);
  const auto header = csv_rows(empty.body).front();
  ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("close", 1, trading::Side::Sell), f.time).decision.ok());
  const std::string note = "Wait, then \"go\"\nReview the exit.";
  ASSERT_TRUE(s.annotate(1, note, {"setup", "patient"}, f.time).decision.ok());
  source.publish(s);
  const auto result = server::handle_api({"GET", "/api/trades.csv?account=main&from=2026-09-22&to=2026-09-22"}, source);
  ASSERT_EQ(result.status, 200) << result.body;
  EXPECT_EQ(result.content_type, "text/csv; charset=utf-8"); EXPECT_EQ(result.download, "trades.csv");
  const auto rows = csv_rows(result.body);
  ASSERT_EQ(rows.size(), 2U); EXPECT_EQ(rows.front(), header);
  const auto row = csv_record(header, rows[1]);
  EXPECT_EQ(row["note"], note); EXPECT_EQ(row["tags"], "setup;patient");
  EXPECT_EQ(row["fees"], "1.300002"); EXPECT_EQ(row["net"], "-21.300002");
  EXPECT_EQ(row["new_york_date"], "2026-09-22"); EXPECT_EQ(row["prices"], "simulated");
  EXPECT_EQ(row["entry_context.equity"], "100000.00");
  const auto trade = json::parse(server::handle_api({"GET", "/api/trades"}, source).body)["trades"][0];
  check_columns(trade, {header.begin(), header.end()});
  const auto filtered = server::handle_api({"GET", "/api/trades.csv?to=2026-09-21"}, source);
  EXPECT_EQ(csv_rows(filtered.body).size(), 1U);
  EXPECT_EQ(csv_rows(filtered.body).front(), header);
  for (const auto* suffix : {"from=2026-02-30", "to=bad", "from=2026-09-23&to=2026-09-22", "from=2026-09-22&from=2026-09-22", "unknown=1"})
    EXPECT_EQ(server::handle_api({"GET", std::string("/api/trades.csv?") + suffix}, source).status, 400);
  EXPECT_EQ(server::handle_api({"GET", "/api/fills.csv?account=missing"}, source).status, 404);
  const auto fills = server::handle_api({"GET", "/api/fills.csv"}, source);
  const auto filled = csv_rows(fills.body);
  ASSERT_EQ(filled.size(), 3U);
  check_columns(json::parse(server::handle_api({"GET", "/api/fills"}, source).body)["fills"][0], {filled[0].begin(), filled[0].end()});
  EXPECT_EQ(csv_record(filled[0], filled[1])["fee"], "0.650001");
}

TEST(TradeReviewApi, GivesBackProfitAndReportsRAndHeatOnlyWithKnownRisk) {
  test::ScriptedMarket f;
  trading::TradingSession s({}, f.time); f.seed(s);
  auto order = f.market("open", 2);
  order.bracket = trading::Bracket{trading::ExitSpec{trading::Trigger{trading::TriggerSource::Option,
      trading::TriggerDirection::AtOrBelow, Money::parse("3")}, {}}, {}};
  ASSERT_TRUE(s.submit(order, f.time).decision.ok());
  f.next(); s.on_quotes({f.quote("6.00", "6.20")}, {f.valuation()}, f.time);
  f.next(); s.on_quotes({f.quote("5.00", "5.20")}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.submit(f.market("close", 2, trading::Side::Sell), f.time).decision.ok());
  ReviewSource source; source.publish(s);
  const auto trade = json::parse(server::handle_api({"GET", "/api/trades"}, source).body)["trades"][0];
  const auto review = trade["review"];
  EXPECT_EQ(review["mae"], "21.30"); EXPECT_EQ(review["mfe"], "378.70");
  EXPECT_EQ(review["give_back"], "221.30"); EXPECT_EQ(review["planned_risk"], "240.00");
  EXPECT_NEAR(review["r_multiple"].get<double>(), 157.4 / 240, 1e-12);
  EXPECT_NEAR(review["heat"].get<double>(), 21.3 / 240, 1e-12);
  ASSERT_TRUE(s.submit(f.market("unplanned"), f.time).decision.ok());
  source.publish(s);
  const auto unknown = json::parse(server::handle_api({"GET", "/api/trades"}, source).body)["trades"][0]["review"];
  EXPECT_TRUE(unknown["planned_risk"].is_null()); EXPECT_TRUE(unknown["r_multiple"].is_null()); EXPECT_TRUE(unknown["heat"].is_null());
}

TEST(TradeReviewApi, CsvDateUsesNewYorkCalendarDateIncludingDst) {
  const json fills = json::array({{{"time", "2026-03-09T01:00:00Z"}, {"id", "1"}}, {{"time", "2026-11-02T01:00:00Z"}, {"id", "2"}}});
  const auto rows = csv_rows(server::paper_csv(fills, true, "practice", "cboe", 1, "", ""));
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(csv_record(rows[0], rows[1])["new_york_date"], "2026-03-08");
  EXPECT_EQ(csv_record(rows[0], rows[2])["new_york_date"], "2026-11-01");
  EXPECT_EQ(csv_record(rows[0], rows[1])["prices"], "price provenance unrecorded; simulated trading");
  EXPECT_EQ(server::csv_quote("a\rb"), "\"a\rb\"");
  const std::vector<std::string> fill_columns = {"account", "account_version", "provider", "prices", "new_york_date",
      "id", "order_id", "symbol", "underlying", "side", "quantity", "price", "fee", "quote_time", "time", "context.spot",
      "context.spot_source", "context.iv", "context.delta", "context.years", "context.equity", "context.floor_room", "context.buying_power"};
  EXPECT_EQ(server::paper_csv_columns(true), fill_columns);
  EXPECT_EQ(server::paper_csv_columns(false).size(), 86U);
}
}  // namespace

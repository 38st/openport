#include <filesystem>
#include <tuple>
#include <gtest/gtest.h>

#include "support/scripted_market.hpp"
#include "trading/state.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
std::string osi(std::string_view compact) { return md::parse_osi(compact)->osi_symbol(); }
const std::string P4900 = osi("SPXW261022P04900000");
const std::string P4890 = osi("SPXW261022P04890000");
SessionConfig config(std::int64_t impact = 0) {
  SessionConfig result;
  result.limits.aggregate = {1e9, 1e9};
  result.limits.per_underlying = {1e9, 1e9};
  result.rules.impact_ticks = impact;
  return result;
}
class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-costs-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
    path = pattern;
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
  std::string file(std::string_view name) const { return (path / name).string(); }
  std::filesystem::path path;
};
/// SPXW puts quoted together; every quote carries `size` on both sides.
struct Chain {
  md::Timestamp time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  std::uint64_t observation = 0;
  void define(TradingSession& s, std::initializer_list<std::string> symbols) {
    for (const auto& symbol : symbols) ASSERT_TRUE(s.define(*md::parse_osi(symbol), time).decision.ok());
  }
  void quote(TradingSession& s, std::vector<std::tuple<std::string, std::string, std::string>> books, Quantity size = 10) {
    ++observation;
    time += md::kNanosPerSecond;
    std::vector<QuoteObservation> quotes;
    std::vector<Valuation> valuations;
    for (const auto& [symbol, bid, ask] : books) {
      const auto contract = *md::parse_osi(symbol);
      quotes.push_back({symbol, observation, time, m(bid), m(ask), size, size});
      valuations.push_back({symbol, time, -0.3, 0.001, 2.0, -0.1, 5000, 5010, 0.99,
                            md::years_between(time, contract.expiry_time()), 0.20, true});
    }
    s.on_quotes(quotes, valuations, time);
  }
};
OrderRequest vertical(std::string client, Quantity units, std::optional<std::string_view> net) {
  OrderRequest r;
  r.client_order_id = std::move(client);
  r.type = net ? OrderType::Limit : OrderType::Market;
  r.tif = net ? TimeInForce::Day : TimeInForce::Ioc;
  r.quantity = units;
  if (net) r.limit_price = m(*net);
  r.legs = {{P4900, Side::Sell, 1}, {P4890, Side::Buy, 1}};
  return r;
}

TEST(FillAudit, FillsKeepTheBookTheyTookAndWhenItWasFirstQuoted) {
  ScriptedMarket market;
  TradingSession session(config(), market.time);
  market.seed(session, "4.00", "4.20", 10);
  const auto first = market.time;
  // The same observation, confirmed five seconds later, keeps its first time.
  market.time += 5 * md::kNanosPerSecond;
  session.on_quotes({market.quote("4.00", "4.20", 10)}, {market.valuation()}, market.time);
  ASSERT_TRUE(session.submit(market.market("buy", 3), market.time).decision.ok());
  ASSERT_TRUE(session.submit(market.market("more", 2), market.time).decision.ok());
  auto fills = session.snapshot()->recent_fills;
  ASSERT_EQ(fills.size(), 2U);
  ASSERT_TRUE(fills[0].quote && fills[1].quote);
  EXPECT_EQ(fills[0].quote->bid, m("4.00"));
  EXPECT_EQ(fills[0].quote->ask, m("4.20"));
  EXPECT_EQ(fills[0].quote->bid_size, 10);
  EXPECT_EQ(fills[0].quote->ask_size, 10);
  EXPECT_EQ(fills[0].quote->left, 10);
  EXPECT_EQ(fills[1].quote->left, 7);
  EXPECT_EQ(fills[0].quote->quoted, first);
  EXPECT_EQ(fills[0].quote_time, market.time);
  EXPECT_EQ(fills[0].observation, 1U);
  // A new observation the provider gave earlier than it was offered keeps that time.
  market.next();
  auto quote = market.quote("4.10", "4.30", 4);
  quote.quoted = market.time - 2 * md::kNanosPerSecond;
  session.on_quotes({quote}, {market.valuation()}, market.time);
  ASSERT_TRUE(session.submit(market.market("sell", 1, Side::Sell), market.time).decision.ok());
  const auto sold = session.snapshot()->recent_fills.back();
  ASSERT_TRUE(sold.quote);
  EXPECT_EQ(sold.quote->quoted, quote.quoted);
  EXPECT_EQ(sold.quote->bid, m("4.10"));
  EXPECT_EQ(sold.quote->left, 4);
  // A quote first given after its own time is refused, as a future-dated one is.
  market.next();
  quote = market.quote();
  quote.quoted = market.time + 1;
  try {
    session.on_quotes({quote}, {market.valuation()}, market.time);
    ADD_FAILURE() << "expected INVALID_TIME";
  } catch (const TradingError& error) { EXPECT_EQ(error.code(), Reason::INVALID_TIME); }
}

TEST(FillAudit, ImpactAndComboFillsRecordEachSlicesBook) {
  ScriptedMarket market;
  TradingSession session(config(1), market.time);
  market.seed(session, "4.00", "4.20", 2);
  ASSERT_TRUE(session.submit(market.market("sweep", 5), market.time).decision.ok());
  const auto fills = session.snapshot()->recent_fills;
  ASSERT_EQ(fills.size(), 3U);
  // Simulated depth beyond the displayed size leaves the budget below zero.
  EXPECT_EQ(fills[0].quote->left, 2);
  EXPECT_EQ(fills[1].quote->left, 0);
  EXPECT_EQ(fills[2].quote->left, -2);

  Chain chain;
  TradingSession spread(config(), chain.time);
  chain.define(spread, {P4900, P4890});
  chain.quote(spread, {{P4900, "2.00", "2.20"}, {P4890, "1.50", "1.65"}}, 6);
  ASSERT_TRUE(spread.submit(vertical("credit", 2, std::nullopt), chain.time).decision.ok());
  const auto legs = spread.snapshot()->recent_fills;
  ASSERT_EQ(legs.size(), 2U);
  for (const auto& fill : legs) {
    ASSERT_TRUE(fill.quote);
    EXPECT_EQ(fill.quote->left, 6);
    EXPECT_EQ(fill.quote->quoted, chain.time);
    EXPECT_EQ(fill.quote->bid, m(fill.symbol == P4900 ? "2.00" : "1.50"));
  }
}

TEST(FillAudit, TheFirstQuotedTimeSurvivesRecoveryAndOlderFillsLoadWithoutIt) {
  TemporaryDirectory directory;
  const auto path = directory.file("audit.jsonl");
  ScriptedMarket market;
  const auto first = market.time;
  std::string continuous;
  {
    TradingSession session(config(), market.time, FileJournal::create(path));
    market.seed(session, "4.00", "4.20", 10);
    market.time += 5 * md::kNanosPerSecond;
    session.on_quotes({market.quote("4.00", "4.20", 10)}, {market.valuation()}, market.time);
    ASSERT_TRUE(session.submit(market.market("buy", 1), market.time).decision.ok());
    continuous = session.snapshot_json();
  }
  auto recovered = TradingSession::recover(FileJournal::read(path), FileJournal::resume(path));
  EXPECT_EQ(recovered.snapshot_json(), continuous);
  // The book recovered with its first time: a later fill on it still reports it.
  market.time += md::kNanosPerSecond;
  recovered.on_quotes({market.quote("4.00", "4.20", 10)}, {market.valuation()}, market.time);
  ASSERT_TRUE(recovered.submit(market.market("again", 1), market.time).decision.ok());
  EXPECT_EQ(recovered.snapshot()->recent_fills.back().quote->quoted, first);
  EXPECT_EQ(recovered.snapshot()->recent_fills.back().quote->left, 9);

  // A fill recorded before fills kept their book loads without one.
  auto json = nlohmann::json::parse(recovered.snapshot_json());
  auto old = json.at("recent_fills").at(0);
  old.erase("quote");
  const auto fill = old.get<Fill>();
  EXPECT_FALSE(fill.quote);
  EXPECT_EQ(nlohmann::json(fill).contains("quote"), false);
}

}  // namespace
}  // namespace openport::trading

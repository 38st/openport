#include "openport/providers/cboe.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace openport;

// A hand-written document with the same shape as Cboe's delayed-quotes JSON.
constexpr std::string_view kChain = R"({
  "timestamp": "2026-09-22 19:48:44",
  "data": {
    "options": [
      {"option": "SPXW260925C07800000", "bid": 41.5, "bid_size": 12.0, "ask": 42.3, "ask_size": 9.0,
       "iv": 0.1432, "open_interest": 1520.0, "volume": 873.0, "delta": 0.4812, "gamma": 0.0021,
       "vega": 3.9102, "theta": -8.7311, "rho": 0.2911, "theo": 41.9},
      {"option": "SPXW260925P07800000", "bid": 63.1, "bid_size": 7.0, "ask": 64.0, "ask_size": 10.0,
       "iv": 0.1511, "open_interest": 2210.0, "volume": 1204.0, "delta": -0.5188, "gamma": 0.0021,
       "vega": 3.9107, "theta": -8.1002, "rho": -0.3120, "theo": 63.5},
      {"option": "SPX261016C08000000", "bid": 70.0, "bid_size": 3.0, "ask": 72.4, "ask_size": 3.0,
       "iv": null, "open_interest": 42.0, "volume": 0.0, "delta": 0.3, "gamma": 0.001,
       "vega": 9.1, "theta": -2.2, "rho": 1.1, "theo": 71.2},
      {"option": "NOT-A-SYMBOL", "bid": 1.0, "ask": 2.0}
    ],
    "symbol": "^SPX", "security_type": "index", "current_price": 7777.27,
    "price_change": 12.5, "bid": 7776.0, "ask": 7778.53
  }
})";

class Collector final : public md::EventSink {
 public:
  void publish(md::Event event) override { events.push_back(std::move(event)); }

  template <typename T>
  std::vector<T> all() const {
    std::vector<T> out;
    for (const auto& event : events) {
      if (const T* e = std::get_if<T>(&event)) out.push_back(*e);
    }
    return out;
  }

  std::vector<md::Event> events;
};

TEST(Cboe, ParsesTheChainDocument) {
  const providers::CboeChain chain = providers::parse_cboe_chain(kChain);
  EXPECT_EQ(chain.symbol, "^SPX");
  EXPECT_DOUBLE_EQ(chain.price, 7777.27);
  EXPECT_DOUBLE_EQ(chain.bid, 7776.0);
  EXPECT_DOUBLE_EQ(chain.ask, 7778.53);
  EXPECT_EQ(md::format_timestamp(chain.as_of), "2026-09-22T19:48:44.000Z");
  ASSERT_EQ(chain.options.size(), 4u);
  EXPECT_EQ(chain.options[0].symbol, "SPXW260925C07800000");
  EXPECT_DOUBLE_EQ(chain.options[0].bid, 41.5);
  EXPECT_DOUBLE_EQ(chain.options[0].ask_size, 9.0);
  EXPECT_DOUBLE_EQ(chain.options[0].iv, 0.1432);
  EXPECT_DOUBLE_EQ(chain.options[1].delta, -0.5188);
  EXPECT_DOUBLE_EQ(chain.options[2].iv, 0.0);  // null becomes zero
}

TEST(Cboe, PublishesDefinitionsBeforeDataAndSkipsBadSymbols) {
  providers::CboeDelayedProvider provider;
  Collector sink;
  provider.publish_chain(providers::parse_cboe_chain(kChain), {}, sink);

  const auto definitions = sink.all<md::ContractDefinition>();
  ASSERT_EQ(definitions.size(), 3u);
  EXPECT_EQ(definitions[0].contract.osi_symbol(), "SPXW  260925C07800000");
  EXPECT_EQ(definitions[2].contract.settlement, md::Settlement::AM);

  // Every event that refers to an id comes after that id's definition.
  std::vector<bool> defined(3, false);
  for (const auto& event : sink.events) {
    if (const auto* d = std::get_if<md::ContractDefinition>(&event)) defined[d->id] = true;
    if (const auto* q = std::get_if<md::OptionQuote>(&event)) EXPECT_TRUE(defined[q->id]);
  }

  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 3u);
  EXPECT_EQ(sink.all<md::OpenInterest>().size(), 3u);
  EXPECT_EQ(sink.all<md::VendorGreeks>().size(), 2u);  // the null-IV row has none

  const auto underlying = sink.all<md::UnderlyingQuote>();
  ASSERT_EQ(underlying.size(), 1u);
  EXPECT_EQ(underlying[0].symbol, "SPX");
  // Delayed data: quotes are stamped 15 minutes before the snapshot.
  EXPECT_EQ(md::format_timestamp(underlying[0].ts), "2026-09-22T19:33:44.000Z");
}

TEST(Cboe, RepublishesOnlyWhatChanged) {
  providers::CboeDelayedProvider provider;
  Collector first;
  providers::CboeChain chain = providers::parse_cboe_chain(kChain);
  provider.publish_chain(chain, {}, first);

  Collector second;
  chain.options[1].bid = 63.2;
  provider.publish_chain(chain, {}, second);
  EXPECT_TRUE(second.all<md::ContractDefinition>().empty());
  const auto quotes = second.all<md::OptionQuote>();
  ASSERT_EQ(quotes.size(), 1u);
  EXPECT_DOUBLE_EQ(quotes[0].bid, 63.2);
  EXPECT_TRUE(second.all<md::OpenInterest>().empty());
  EXPECT_EQ(second.all<md::UnderlyingQuote>().size(), 1u);  // spot is always republished
}

TEST(Cboe, AppliesExpiryAndStrikeFilters) {
  providers::CboeDelayedProvider provider;
  Collector sink;
  md::Subscription nearest;
  nearest.max_expiries = 1;
  provider.publish_chain(providers::parse_cboe_chain(kChain), nearest, sink);
  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 2u);  // only 2026-09-25

  providers::CboeDelayedProvider narrow_provider;
  Collector narrow;
  md::Subscription window;
  window.strike_window = 0.01;  // 7800 is within 1% of 7777; 8000 is not
  narrow_provider.publish_chain(providers::parse_cboe_chain(kChain), window, narrow);
  EXPECT_EQ(narrow.all<md::ContractDefinition>().size(), 2u);
}

TEST(Cboe, IndexChainsUseAnUnderscore) {
  EXPECT_EQ(providers::cboe_chain_url("SPX"),
            "https://cdn.cboe.com/api/global/delayed_quotes/options/_SPX.json");
  EXPECT_EQ(providers::cboe_chain_url("SPY"),
            "https://cdn.cboe.com/api/global/delayed_quotes/options/SPY.json");
}

TEST(Cboe, UpdatesKnownContractsOutsideTheWindowAndRetiresOnlyTheirUnderlying) {
  providers::CboeDelayedProvider provider;
  auto chain = providers::parse_cboe_chain(kChain);
  Collector sink;
  md::Subscription sub;
  sub.strike_window = 0.01;
  provider.publish_chain(chain, sub, sink);
  sink.events.clear();
  chain.price = 10000;
  chain.options[0].bid = 42;
  provider.publish_chain(chain, sub, sink);
  ASSERT_EQ(sink.all<md::OptionQuote>().size(), 1u);
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].bid, 42);
  sink.events.clear();
  auto other = chain;
  other.symbol = "SPY";
  other.options.clear();
  provider.publish_chain(other, sub, sink);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  chain.options.clear();
  provider.publish_chain(chain, sub, sink);
  ASSERT_EQ(sink.all<md::OptionQuote>().size(), 2u);
  for (const auto& q : sink.all<md::OptionQuote>()) EXPECT_EQ(q.bid + q.ask, 0);
}

}  // namespace

namespace {
TEST(Cboe, FrozenAfterHoursQuotesUseLastTradeTimeRatherThanAdvancingPublicationTime) {
  // Publication is UTC; last_trade_time is New York local (EDT here).
  const auto chain = providers::parse_cboe_chain(R"({
    "timestamp":"2026-09-22 20:34:59",
    "data":{"options":[{"option":"SPXW260925C07800000","bid":41,"ask":42,"iv":0.2}],
            "symbol":"^SPX","current_price":7777,"last_trade_time":"2026-09-22 16:14:59"}
  })");
  providers::CboeDelayedProvider provider;
  Collector sink;
  provider.publish_chain(chain, {}, sink);
  const auto expected = *md::parse_datetime("2026-09-22 16:14:59", md::Zone::NewYork);
  EXPECT_EQ(sink.all<md::UnderlyingQuote>().at(0).ts, expected);
  EXPECT_EQ(sink.all<md::OptionQuote>().at(0).ts, expected);
  EXPECT_EQ(sink.all<md::VendorGreeks>().at(0).ts, expected);
}

TEST(Cboe, LastTradeTimeCannotAdvanceDelayedSnapshotAndInvalidTimeFallsBack) {
  for (const auto* last_trade : {"2026-09-22 16:34:59", "invalid", ""}) {
    const auto chain = providers::parse_cboe_chain(
        std::string(
            R"({"timestamp":"2026-09-22 20:34:59","data":{"options":[],"symbol":"SPY","last_trade_time":")") +
        last_trade + R"("}})");
    providers::CboeDelayedProvider provider;
    Collector sink;
    provider.publish_chain(chain, {}, sink);
    EXPECT_EQ(sink.all<md::UnderlyingQuote>().at(0).ts,
              *md::parse_datetime("2026-09-22 20:19:59", md::Zone::Utc));
  }
}
}  // namespace

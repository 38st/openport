#include "openport/providers/massive.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

using namespace openport;

// A hand-written page shaped like Massive's /v3/snapshot/options response, with the
// fields in a different order from the documentation to check order independence.
constexpr std::string_view kPage = R"({
  "status": "OK",
  "request_id": "abc",
  "results": [
    {
      "greeks": {"delta": -0.0484, "gamma": 0.0004, "theta": -0.8494, "vega": 1.5783},
      "details": {"contract_type": "put", "exercise_style": "european",
                  "expiration_date": "2026-10-05", "shares_per_contract": 100,
                  "strike_price": 7405, "ticker": "O:SPXW261005P07405000"},
      "implied_volatility": 0.1575,
      "last_quote": {"ask": 4.6, "ask_size": 172, "bid": 4.4, "bid_size": 314,
                     "last_updated": 1790103279803625000, "midpoint": 4.5, "timeframe": "REAL-TIME"},
      "open_interest": 7,
      "underlying_asset": {"price": 7777.27, "ticker": "I:SPX", "last_updated": 1790103279000000000,
                           "timeframe": "REAL-TIME"}
    },
    {
      "details": {"ticker": "O:SPXW261005C09000000", "exercise_style": "european",
                  "shares_per_contract": 100},
      "last_quote": {"bid": 0.05, "ask": 0.1, "bid_size": 10, "ask_size": 20,
                     "last_updated": 1790103279000000000, "timeframe": "REAL-TIME"},
      "open_interest": 1200
    }
  ],
  "next_url": "https://api.massive.com/v3/snapshot/options/I:SPX?cursor=YXA9"
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

TEST(Massive, ParsesAPageInAnyFieldOrder) {
  const providers::MassivePage page = providers::parse_massive_chain_page(kPage);
  ASSERT_EQ(page.contracts.size(), 2u);
  EXPECT_EQ(page.next_url, "https://api.massive.com/v3/snapshot/options/I:SPX?cursor=YXA9");
  EXPECT_DOUBLE_EQ(page.underlying_price, 7777.27);

  const auto& put = page.contracts[0];
  EXPECT_EQ(put.symbol, "SPXW261005P07405000");
  EXPECT_TRUE(put.european);
  EXPECT_TRUE(put.has_quote);
  EXPECT_TRUE(put.realtime);
  EXPECT_DOUBLE_EQ(put.bid, 4.4);
  EXPECT_DOUBLE_EQ(put.ask_size, 172.0);
  EXPECT_EQ(put.quote_ts, 1790103279803625000);  // kept exact, not rounded through a double
  EXPECT_DOUBLE_EQ(put.iv, 0.1575);
  EXPECT_DOUBLE_EQ(put.vega, 1.5783);
  EXPECT_DOUBLE_EQ(put.open_interest, 7.0);

  EXPECT_DOUBLE_EQ(page.contracts[1].iv, 0.0);  // no greeks for this one
}

TEST(Massive, ErrorResponsesThrow) {
  EXPECT_THROW((void)providers::parse_massive_chain_page(
                   R"({"status":"ERROR","request_id":"x","error":"Unknown API Key"})"),
               std::runtime_error);
  EXPECT_THROW((void)providers::parse_massive_chain_page(
                   R"({"status":"NOT_AUTHORIZED","message":"You are not entitled to this data."})"),
               std::runtime_error);
}

TEST(Massive, PublishesTheChainWithRhoMarkedUnpublished) {
  providers::MassiveProvider provider({.api_key = "test"});
  Collector sink;
  const providers::MassivePage page = providers::parse_massive_chain_page(kPage);
  provider.publish_chain("SPX", page.contracts, page.underlying_price, page.underlying_ts, {}, sink);

  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 2u);
  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 2u);
  EXPECT_EQ(sink.all<md::OpenInterest>().size(), 2u);
  const auto greeks = sink.all<md::VendorGreeks>();
  ASSERT_EQ(greeks.size(), 1u);
  EXPECT_DOUBLE_EQ(greeks[0].theta, -0.8494);
  EXPECT_TRUE(std::isnan(greeks[0].rho));
  EXPECT_EQ(sink.all<md::UnderlyingQuote>().at(0).symbol, "SPX");
}

TEST(Massive, IndexUnderlyingsUseTheIPrefix) {
  EXPECT_EQ(providers::massive_chain_url("https://api.massive.com", "SPX"),
            "https://api.massive.com/v3/snapshot/options/I:SPX?limit=250");
  EXPECT_EQ(providers::massive_chain_url("https://api.massive.com", "SPY"),
            "https://api.massive.com/v3/snapshot/options/SPY?limit=250");
}

TEST(Massive, RequiresAKey) {
  EXPECT_THROW(providers::MassiveProvider({}), std::invalid_argument);
}

}  // namespace

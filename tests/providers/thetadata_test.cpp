#include "openport/providers/thetadata.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using namespace openport;

constexpr std::string_view kQuotes =
    R"({"timestamp":"2026-09-22T15:33:42.125","symbol":"SPXW","expiration":"2026-10-05","strike":7405.0,"right":"put","bid_size":314,"bid_exchange":5,"bid":4.4,"bid_condition":50,"ask_size":172,"ask_exchange":5,"ask":4.6,"ask_condition":50}
{"timestamp":"2026-09-22T15:33:42.125","symbol":"SPXW","expiration":"20261005","strike":7800.0,"right":"call","bid_size":12,"bid":41.5,"ask_size":9,"ask":42.3}
{"timestamp":"2026-09-22T15:33:41.000","symbol":"SPX","expiration":"2026-10-16","strike":8000.0,"right":"call","bid":70.0,"ask":72.4,"bid_size":3,"ask_size":3}
)";

constexpr std::string_view kImpliedVols =
    R"({"symbol":"SPXW","expiration":"2026-10-05","strike":7405.0,"right":"put","timestamp":"2026-09-22T15:33:42.125","bid":4.4,"ask":4.6,"implied_vol":0.1575,"iv_error":0.0001,"underlying_timestamp":"2026-09-22T15:33:42.100","underlying_price":7777.27}
)";

constexpr std::string_view kOpenInterest =
    R"({"symbol":"SPXW","expiration":"2026-10-05","strike":7405.0,"right":"put","timestamp":"2026-09-22T06:30:00.000","open_interest":1520}
)";

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

TEST(ThetaData, ParsesNdjsonRows) {
  const auto rows = providers::parse_theta_rows(kQuotes);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].root, "SPXW");
  EXPECT_EQ(rows[0].expiry, (md::Date{2026, 10, 5}));
  EXPECT_EQ(rows[1].expiry, (md::Date{2026, 10, 5}));  // YYYYMMDD also accepted
  EXPECT_DOUBLE_EQ(rows[0].strike, 7405.0);
  EXPECT_EQ(rows[0].type, pricing::OptionType::Put);
  EXPECT_DOUBLE_EQ(rows[0].bid, 4.4);
  EXPECT_DOUBLE_EQ(rows[0].ask_size, 172.0);
  // New York wall clock, with milliseconds.
  EXPECT_EQ(md::format_timestamp(rows[0].ts), "2026-09-22T19:33:42.125Z");
}

TEST(ThetaData, EmptyResponsesParseToNothing) {
  EXPECT_TRUE(providers::parse_theta_rows("").empty());
  EXPECT_TRUE(providers::parse_theta_rows("\n").empty());
}

TEST(ThetaData, MergesQuotesImpliedVolsAndOpenInterest) {
  providers::ThetaDataProvider provider;
  Collector sink;
  provider.publish_chain("SPX", providers::parse_theta_rows(kQuotes),
                         providers::parse_theta_rows(kImpliedVols),
                         providers::parse_theta_rows(kOpenInterest), {}, sink);

  const auto definitions = sink.all<md::ContractDefinition>();
  ASSERT_EQ(definitions.size(), 3u);
  EXPECT_EQ(definitions[0].contract.osi_symbol(), "SPXW  261005P07405000");
  EXPECT_EQ(definitions[2].contract.settlement, md::Settlement::AM);  // monthly SPX

  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 3u);
  const auto greeks = sink.all<md::VendorGreeks>();
  ASSERT_EQ(greeks.size(), 1u);
  EXPECT_DOUBLE_EQ(greeks[0].iv, 0.1575);
  EXPECT_TRUE(std::isnan(greeks[0].vega));  // undocumented units are not guessed

  const auto oi = sink.all<md::OpenInterest>();
  ASSERT_EQ(oi.size(), 1u);
  EXPECT_DOUBLE_EQ(oi[0].contracts, 1520.0);

  const auto spot = sink.all<md::UnderlyingQuote>();
  ASSERT_EQ(spot.size(), 1u);
  EXPECT_DOUBLE_EQ(spot[0].last, 7777.27);
}

}  // namespace

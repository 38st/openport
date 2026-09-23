#include "openport/providers/cboe.hpp"

#include <gtest/gtest.h>

#include <string>
#include <tuple>
#include <vector>

#include "support/http_stub.hpp"

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
    "price_change": 12.5, "bid": 7776.0, "ask": 7778.53,
    "last_trade_time": "2026-09-22 15:33:42"
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
    if (const auto* q = std::get_if<md::OptionQuote>(&event)) {
      EXPECT_TRUE(defined[q->id]);
    }
  }

  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 3u);
  EXPECT_EQ(sink.all<md::OpenInterest>().size(), 3u);
  EXPECT_EQ(sink.all<md::VendorGreeks>().size(), 2u);  // the null-IV row has none

  const auto underlying = sink.all<md::UnderlyingQuote>();
  ASSERT_EQ(underlying.size(), 1u);
  EXPECT_EQ(underlying[0].symbol, "SPX");
  // Underlying and option clocks are independent.
  EXPECT_EQ(md::format_timestamp(underlying[0].ts), "2026-09-22T19:33:42.000Z");
  EXPECT_EQ(md::format_timestamp(sink.all<md::OptionQuote>()[0].ts), "2026-09-22T19:33:44.000Z");
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
TEST(Cboe, CurbQuotesAdvancePastTheUnderlyingLastTrade) {
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
  const auto option_time = *md::parse_datetime("2026-09-22 16:19:59", md::Zone::NewYork);
  EXPECT_EQ(sink.all<md::OptionQuote>().at(0).ts, option_time);
  EXPECT_EQ(sink.all<md::VendorGreeks>().at(0).ts, option_time);
}

TEST(Cboe, UnderlyingRetainsItsOwnClockAndUnknownTimeStaysUnknown) {
  for (const auto* last_trade : {"2026-09-22 16:34:59", "invalid", ""}) {
    const auto chain = providers::parse_cboe_chain(
        std::string(
            R"({"timestamp":"2026-09-22 20:34:59","data":{"options":[],"symbol":"SPY","last_trade_time":")") +
        last_trade + R"("}})");
    providers::CboeDelayedProvider provider;
    Collector sink;
    provider.publish_chain(chain, {}, sink);
    EXPECT_EQ(sink.all<md::UnderlyingQuote>().at(0).ts,
              md::parse_datetime(last_trade, md::Zone::NewYork).value_or(0));
  }
}
}  // namespace

namespace {
TEST(Cboe, OptionMarketTimeTracksEachProductsSessionsAndNeverRunsPastDelayedTime) {
  for (auto symbol : {"SPXW", "SPY", "AAPL"}) {
    for (const auto& [date, hour, minute] :
         {std::tuple{md::Date{2026, 9, 22}, 21, 25}, std::tuple{md::Date{2026, 9, 22}, 17, 10},
          std::tuple{md::Date{2026, 9, 23}, 9, 42}, std::tuple{md::Date{2026, 9, 26}, 21, 25},
          std::tuple{md::Date{2026, 11, 27}, 14, 0}}) {
      providers::CboeChain chain;
      chain.symbol = md::conventions_for_root(symbol).underlying;
      chain.as_of = md::new_york_to_utc(date, hour, minute);
      chain.last_trade_time = md::new_york_to_utc({2026, 9, 22}, 16, 0);
      chain.price = 100;
      chain.options.push_back({std::string(symbol) + "261218C00100000", 1, 1, 2, 1, .2});
      providers::CboeDelayedProvider provider;
      Collector sink;
      provider.publish_chain(chain, {}, sink);
      const auto delayed = chain.as_of - 15 * md::kNanosPerMinute;
      const auto expected = md::trading_session(symbol, delayed).market_time;
      EXPECT_EQ(sink.all<md::UnderlyingQuote>().at(0).ts, chain.last_trade_time);
      EXPECT_EQ(sink.all<md::OptionQuote>().at(0).ts, expected);
      EXPECT_LE(expected, delayed);
      if (hour == 21 && date == md::Date{2026, 9, 22}) {
        EXPECT_EQ(expected,
                  std::string_view(symbol) == "SPXW"
                      ? delayed
                      : md::new_york_to_utc(date, 16, std::string_view(symbol) == "SPY" ? 15 : 0));
        // An overnight changed quote must advance even while current_price and
        // last_trade_time stay frozen; SnapshotPublisher still deduplicates.
        chain.as_of += 2 * md::kNanosPerMinute;
        chain.options[0].bid += .1;
        sink.events.clear();
        provider.publish_chain(chain, {}, sink);
        ASSERT_EQ(sink.all<md::OptionQuote>().size(), 1u);
        EXPECT_EQ(sink.all<md::OptionQuote>()[0].ts,
                  expected + (std::string_view(symbol) == "SPXW" ? 2 * md::kNanosPerMinute : 0));
      }
    }
  }
}

// Cboe's chart files: intraday prices are numbers, daily prices are strings.
constexpr std::string_view kIntraday = R"({"timestamp": "2026-09-22 20:14:23", "symbol": "_SPX", "data": [
  {"datetime": "2026-09-22T09:32:00", "sequence_number": 2,
   "price": {"open": 7772.9399, "high": 7779.8799, "low": 7772.8999, "close": 7779.1499},
   "volume": {"stock_volume": 0, "total_options_volume": 25886}},
  {"datetime": "2026-09-22T09:31:00", "sequence_number": 1,
   "price": {"open": 7770.8101, "high": 7773.5801, "low": 7769.6001, "close": 7772.6499}},
  {"datetime": "2026-09-22T09:33:00", "price": {"open": 0, "high": 0, "low": 0, "close": 0}},
  {"datetime": "not a time", "price": {"open": 1, "high": 1, "low": 1, "close": 1}},
  {"datetime": "2026-09-22T09:34:00", "price": {"open": "7779.5", "high": "7780.68", "low": "7778.11", "close": "7779.2"}}
]})";

constexpr std::string_view kDaily = R"({"timestamp": "2026-09-23 02:02:04", "symbol": "_SPX", "data": [
  {"date": "1975-01-02", "volume": "0.0", "open": "0.000000", "high": "70.920000", "low": "68.650000", "close": "70.230000"},
  {"date": "2026-09-22", "volume": "0.0", "open": "7770.810000", "high": "7782.190000", "low": "7756.260000", "close": "7764.640000"},
  {"date": "2026-09-21", "volume": "0.0", "open": "7692.830000", "high": "7779.220000", "low": "7691.190000", "close": "7764.700000"},
  {"date": "2026-02-30", "open": "1", "high": "1", "low": "1", "close": "1"}
]})";

TEST(CboeCharts, UrlsFollowTheChainConvention) {
  EXPECT_EQ(providers::cboe_chart_url("SPX", providers::CboeChart::Intraday),
            "https://cdn.cboe.com/api/global/delayed_quotes/charts/intraday/_SPX.json");
  EXPECT_EQ(providers::cboe_chart_url("SPY", providers::CboeChart::Daily),
            "https://cdn.cboe.com/api/global/delayed_quotes/charts/historical/SPY.json");
}

TEST(CboeCharts, IntradayBarsStartAMinuteBeforeTheirLabel) {
  const auto bars = providers::parse_cboe_intraday(kIntraday);
  ASSERT_EQ(bars.size(), 3u);
  EXPECT_EQ(bars[0], (md::Bar{md::new_york_to_utc({2026, 9, 22}, 9, 30), 7770.8101, 7773.5801,
                              7769.6001, 7772.6499}));
  EXPECT_EQ(bars[1].start, md::new_york_to_utc({2026, 9, 22}, 9, 31));
  EXPECT_EQ(bars[2], (md::Bar{md::new_york_to_utc({2026, 9, 22}, 9, 33), 7779.5, 7780.68, 7778.11,
                              7779.2}));
  EXPECT_THROW((void)providers::parse_cboe_intraday(R"({"data": {}})"), std::runtime_error);
}

TEST(CboeCharts, DailyBarsStartAtTheOpenAndSkipRowsWithoutOne) {
  const auto bars = providers::parse_cboe_daily(kDaily);
  ASSERT_EQ(bars.size(), 2u);
  EXPECT_EQ(bars[0], (md::Bar{md::new_york_to_utc({2026, 9, 21}, 9, 30), 7692.83, 7779.22, 7691.19,
                              7764.70}));
  EXPECT_EQ(bars[1].start, md::new_york_to_utc({2026, 9, 22}, 9, 30));
  EXPECT_EQ(bars[1].close, 7764.64);
  EXPECT_THROW((void)providers::parse_cboe_daily("{}"), std::runtime_error);
}

TEST(CboeCharts, HistoryFetchesWhatIsDueAndBacksOffFromMissingFiles) {
  md::Timestamp clock = md::new_york_to_utc({2026, 9, 22}, 11, 0);  // regular session
  std::vector<std::tuple<std::string, providers::CboeChart, std::size_t>> received;
  providers::CboeChartHistory::Options options;
  options.clock = [&] { return clock; };
  providers::CboeChartHistory history(
      {"SPX", "XYZ"},
      [&](const std::string& symbol, providers::CboeChart chart, std::vector<md::Bar> bars) {
        received.emplace_back(symbol, chart, bars.size());
      },
      options);
  test::HttpStub http;
  http.respond = [](std::string_view url) {
    if (url.find("XYZ") != std::string_view::npos) return net::HttpResponse{403, "denied"};
    if (url.find("intraday") != std::string_view::npos) return net::HttpResponse{200, std::string(kIntraday)};
    return net::HttpResponse{200, std::string(kDaily)};
  };
  auto next = history.poll_once(http);
  EXPECT_EQ(http.urls.size(), 4u);
  ASSERT_EQ(received.size(), 2u);
  EXPECT_EQ(received[0], std::make_tuple(std::string("SPX"), providers::CboeChart::Intraday, std::size_t{3}));
  EXPECT_EQ(received[1], std::make_tuple(std::string("SPX"), providers::CboeChart::Daily, std::size_t{2}));
  EXPECT_EQ(next, clock + md::kNanosPerMinute);
  EXPECT_NE(history.error().find("cboe XYZ minute bars: Cboe publishes no chart (HTTP 403)"), std::string::npos);

  // A minute later only the session's minute bars are due; XYZ waits an hour.
  clock += md::kNanosPerMinute;
  next = history.poll_once(http);
  EXPECT_EQ(http.urls.size(), 5u);
  EXPECT_EQ(http.urls.back(), providers::cboe_chart_url("SPX", providers::CboeChart::Intraday));

  // Overnight the minute bars refresh every 15 minutes; a bad document is an error, not a crash.
  clock = md::new_york_to_utc({2026, 9, 22}, 22, 0);
  http.respond = [](std::string_view) { return net::HttpResponse{200, "{}"}; };
  next = history.poll_once(http);
  EXPECT_EQ(next, clock + md::kNanosPerMinute) << "a failure retries after a minute";
  EXPECT_NE(history.error().find("cboe SPX minute bars: Cboe intraday chart: missing data array"), std::string::npos);
  http.respond = [](std::string_view url) {
    return net::HttpResponse{200, std::string(url.find("intraday") != std::string_view::npos ? kIntraday : kDaily)};
  };
  clock += md::kNanosPerMinute;
  next = history.poll_once(http);
  EXPECT_EQ(history.error().find("SPX"), std::string::npos) << history.error();
  EXPECT_EQ(next, clock + 15 * md::kNanosPerMinute);
}
}  // namespace

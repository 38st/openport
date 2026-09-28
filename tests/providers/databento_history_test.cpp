#include "openport/providers/databento_history.hpp"

#include <databento/constants.hpp>
#include <databento/record.hpp>
#include <gtest/gtest.h>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

#include "support/recording.hpp"

namespace {
using namespace openport;
namespace db = databento;
using nlohmann::json;
db::UnixNanos at(md::Timestamp time) { return db::UnixNanos{db::UnixNanos::duration{static_cast<std::uint64_t>(time)}}; }
md::Timestamp time_of(const json& row, const char* field) { return *md::parse_datetime(row.at(field).get<std::string>(), md::Zone::Utc); }
template <typename T>
void stamp(T& record, db::RType type, const json& row, md::Timestamp time) {
  record.hd.length = static_cast<std::uint8_t>(sizeof(T) / db::RecordHeader::kLengthMultiplier);
  record.hd.rtype = type;
  record.hd.instrument_id = row.at("instrument_id").get<std::uint32_t>();
  record.hd.ts_event = at(time);
  if constexpr (requires { record.ts_recv; }) record.ts_recv = at(time);
}
json fixture() {
  std::ifstream input(std::filesystem::path(OPENPORT_TEST_DATA_DIR) / "history/databento.json");
  return json::parse(input);
}
void deliver(const json& data, const providers::DatabentoHistoryRequest& request,
             const db::RecordCallback& callback) {
  if (request.schema == db::Schema::Definition) {
    for (const auto& row : data.at("definitions")) {
      db::InstrumentDefMsg record{};
      stamp(record, db::RType::InstrumentDef, row, request.window.begin);
      const auto symbol = row.at("raw_symbol").get<std::string>();
      std::strncpy(record.raw_symbol.data(), symbol.c_str(), record.raw_symbol.size() - 1);
      record.instrument_class = symbol[12] == 'C' ? db::InstrumentClass::Call : db::InstrumentClass::Put;
      callback(db::Record{&record.hd});
    }
    return;
  }
  const auto key = request.schema == db::Schema::Cbbo1S ? "cbbo-1s" : request.schema == db::Schema::Trades ? "trades" : "statistics";
  for (const auto& row : data.at(key)) {
    const auto time = time_of(row, request.schema == db::Schema::Cbbo1S ? "ts_recv" : "ts_event");
    const auto index_time = row.contains("ts_recv") ? time_of(row, "ts_recv") : time;
    if (index_time < request.window.begin || index_time >= request.window.end) continue;
    if (request.schema == db::Schema::Cbbo1S) {
      db::CbboMsg record{};
      stamp(record, db::RType::Cbbo1S, row, time);
      // CBBO must use ts_recv even if the last trade's event time is stale.
      record.hd.ts_event = at(time - md::kNanosPerDay);
      record.levels[0].bid_px = row.at("bid_px").get<std::int64_t>();
      record.levels[0].ask_px = row.at("ask_px").get<std::int64_t>();
      record.levels[0].bid_sz = row.at("bid_sz").get<std::uint32_t>();
      record.levels[0].ask_sz = row.at("ask_sz").get<std::uint32_t>();
      callback(db::Record{&record.hd});
    } else if (request.schema == db::Schema::Trades) {
      db::TradeMsg record{};
      stamp(record, db::RType::Mbp0, row, time);
      record.ts_recv = at(index_time);
      record.price = row.at("price").get<std::int64_t>();
      record.size = row.at("size").get<std::uint32_t>();
      callback(db::Record{&record.hd});
    } else {
      db::StatMsg record{};
      stamp(record, db::RType::Statistics, row, time);
      record.ts_recv = at(index_time);
      record.stat_type = db::StatType::OpenInterest;
      record.quantity = row.at("quantity").get<std::int64_t>();
      callback(db::Record{&record.hd});
    }
  }
}

TEST(DatabentoHistory, SavedSdkRecordsUseParentsMarketTimeParityAndBoundedRequests) {
  test::RecordingFile file;
  const auto data = fixture();
  std::size_t requests = 0;
  auto source = providers::databento_history([&](const auto& query, const auto& callback) {
    ++requests;
    EXPECT_EQ(query.symbols, (std::vector<std::string>{"SPX.OPT", "SPXW.OPT"}));
    if (query.schema != db::Schema::Definition && query.schema != db::Schema::Statistics) {
      EXPECT_EQ(query.window.end - query.window.begin, 15 * md::kNanosPerSecond);
    }
    deliver(data, query, callback);
  });
  const providers::ImportDay input{"databento", {2026, 9, 22}, {{"SPX"}, 1, .05}, file.directory};
  const auto path = providers::import_day(input, *source, {2026, 9, 27});
  md::RecordingReader reader(path);
  EXPECT_TRUE(reader.header().imported);
  EXPECT_EQ(reader.header().provider, "databento");
  EXPECT_EQ(reader.header().started, md::new_york_to_utc({2026, 9, 21}, 20, 15));
  std::set<md::InstrumentId> defined;
  std::vector<md::OptionQuote> quotes;
  std::vector<md::OptionTrade> trades;
  std::vector<md::OpenInterest> interest;
  std::vector<md::OptionVolume> volume;
  while (const auto event = reader.next()) {
    if (const auto* definition = std::get_if<md::ContractDefinition>(&event->event)) {
      EXPECT_EQ(definition->contract.expiry, (md::Date{2026, 9, 23}));
      EXPECT_EQ(definition->contract.strike, 5000);
      EXPECT_EQ(definition->contract.settlement, md::Settlement::PM);
      defined.insert(definition->id);
    }
    if (const auto* quote = std::get_if<md::OptionQuote>(&event->event)) {
      EXPECT_TRUE(defined.contains(quote->id));
      quotes.push_back(*quote);
    }
    if (const auto* trade = std::get_if<md::OptionTrade>(&event->event)) {
      trades.push_back(*trade);
      if (trades.size() == 2) { EXPECT_EQ(event->received, reader.header().started + 1'500'000'000); }
      if (trades.size() == 4) { EXPECT_EQ(event->received, reader.header().started + 30'100'000'000); }
    }
    if (const auto* oi = std::get_if<md::OpenInterest>(&event->event)) interest.push_back(*oi);
    if (const auto* total = std::get_if<md::OptionVolume>(&event->event)) volume.push_back(*total);
    EXPECT_FALSE(std::holds_alternative<md::UnderlyingQuote>(event->event));
  }
  ASSERT_EQ(quotes.size(), 3U);
  EXPECT_EQ(quotes[0].ts, reader.header().started);
  EXPECT_EQ(quotes.back().ts, reader.header().started + 15 * md::kNanosPerSecond);
  EXPECT_DOUBLE_EQ(quotes[0].bid, 5.0);
  ASSERT_EQ(trades.size(), 4U);
  EXPECT_EQ(trades[0].ts, reader.header().started + 1'125'000'000);
  EXPECT_EQ(trades[1].ts, reader.header().started + 700'000'000);
  EXPECT_EQ(trades[2].ts, reader.header().started + 15 * md::kNanosPerSecond - 1);
  EXPECT_EQ(trades[3].ts, reader.header().started + md::kNanosPerSecond / 2);
  ASSERT_EQ(interest.size(), 1U);
  EXPECT_EQ(interest[0].contracts, 1200);
  ASSERT_EQ(volume.size(), 4U);
  EXPECT_EQ(volume[0].contracts, 3);
  EXPECT_EQ(volume[1].contracts, 4);
  EXPECT_EQ(volume[1].ts, volume[0].ts);
  EXPECT_EQ(volume[2].contracts, 6);
  EXPECT_EQ(volume[3].contracts, 7);
  EXPECT_EQ(volume[3].ts, volume[2].ts);
  EXPECT_GT(requests, 1000U);
  EXPECT_TRUE(reader.diagnostic().empty());
}

TEST(DatabentoHistory, MissingDefinitionOrProviderErrorFailsWithoutPublishingAPartialDay) {
  test::RecordingFile file;
  auto source = providers::databento_history([](const auto& query, const auto& callback) {
    if (query.schema == db::Schema::Cbbo1S) {
      db::CbboMsg record{};
      stamp(record, db::RType::Cbbo1S, json{{"instrument_id", 42}}, query.window.begin);
      callback(db::Record{&record.hd});
    }
  });
  const providers::ImportDay input{"databento", {2026, 9, 22}, {{"SPX"}}, file.directory};
  EXPECT_THROW((void)providers::import_day(input, *source, {2026, 9, 27}), std::runtime_error);
  EXPECT_TRUE(std::filesystem::is_empty(file.directory));
}

TEST(DatabentoHistory, PriorBusinessDayInterestIsAvailableOvernightWithoutFutureObservations) {
  test::RecordingFile file;
  auto data = fixture();
  data["statistics"] = json::array({data["statistics"][0]});
  data["statistics"][0]["ts_event"] = "2026-09-21T10:30:00Z";
  data["statistics"][0]["quantity"] = 1000;
  data["statistics"].push_back({{"instrument_id", 42}, {"ts_event", "2026-09-22T10:30:00Z"}, {"ts_recv", "2026-09-22T10:30:05Z"}, {"quantity", 1200}});
  auto source = providers::databento_history([&](const auto& query, const auto& callback) { deliver(data, query, callback); });
  const providers::ImportDay input{"databento", {2026, 9, 22}, {{"SPX"}, 1, .05}, file.directory};
  const auto path = providers::import_day(input, *source, {2026, 9, 27});
  md::RecordingReader reader(path);
  std::vector<md::RecordedEvent> interest;
  while (const auto event = reader.next())
    if (std::holds_alternative<md::OpenInterest>(event->event)) interest.push_back(*event);
  ASSERT_EQ(interest.size(), 2U);
  EXPECT_EQ(std::get<md::OpenInterest>(interest[0].event).contracts, 1000);
  EXPECT_EQ(std::get<md::OpenInterest>(interest[0].event).ts, time_of(data["statistics"][0], "ts_event"));
  EXPECT_EQ(interest[0].received, reader.header().started);
  EXPECT_EQ(std::get<md::OpenInterest>(interest[1].event).contracts, 1200);
  EXPECT_EQ(std::get<md::OpenInterest>(interest[1].event).ts, time_of(data["statistics"][1], "ts_event"));
  EXPECT_EQ(interest[1].received, time_of(data["statistics"][1], "ts_recv"));
}

TEST(DatabentoHistory, VendorIdsCanChangeBetweenUtcDaysWithoutChangingRecordingIdentity) {
  const auto first = md::new_york_to_utc({2026, 11, 25}, 20, 15);
  const auto second = md::new_york_to_utc({2026, 11, 26}, 20, 15);
  auto source = providers::databento_history([&](const auto& query, const auto& callback) {
    const bool later = query.window.begin / md::kNanosPerDay == second / md::kNanosPerDay;
    if (query.schema == db::Schema::Definition) {
      for (const bool put : {false, true}) {
        db::InstrumentDefMsg record{};
        const auto id = (put != later) ? 43 : 42;
        stamp(record, db::RType::InstrumentDef, json{{"instrument_id", id}}, query.window.begin);
        const auto symbol = put ? "SPXW  261127P05000000" : "SPXW  261127C05000000";
        std::strncpy(record.raw_symbol.data(), symbol, record.raw_symbol.size() - 1);
        record.instrument_class = put ? db::InstrumentClass::Put : db::InstrumentClass::Call;
        callback(db::Record{&record.hd});
      }
    } else if (query.schema == db::Schema::Cbbo1S) {
      db::CbboMsg record{};
      stamp(record, db::RType::Cbbo1S, json{{"instrument_id", later ? 43 : 42}}, query.window.begin);
      record.levels[0].bid_px = 5 * db::kFixedPriceScale;
      record.levels[0].ask_px = 6 * db::kFixedPriceScale;
      callback(db::Record{&record.hd});
    }
  });
  test::EventCollector collector;
  source->prepare({"databento", {2026, 11, 27}, {{"SPX"}}, "unused"}, {first, second + md::kNanosPerMinute}, collector);
  source->read({first, first + 15 * md::kNanosPerSecond}, collector);
  source->read({second, second + 15 * md::kNanosPerSecond}, collector);
  std::size_t definitions = 0;
  std::vector<md::OptionQuote> quotes;
  for (const auto& event : collector.snapshot()) {
    if (std::holds_alternative<md::ContractDefinition>(event)) ++definitions;
    if (const auto* quote = std::get_if<md::OptionQuote>(&event)) quotes.push_back(*quote);
  }
  EXPECT_EQ(definitions, 2U);
  ASSERT_EQ(quotes.size(), 2U);
  EXPECT_EQ(quotes[0].id, quotes[1].id);
  EXPECT_EQ(quotes[1].ts, second);
}

}  // namespace

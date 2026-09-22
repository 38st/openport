#include "openport/providers/databento.hpp"

#include <databento/constants.hpp>
#include <databento/record.hpp>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

using namespace openport;
namespace db = databento;

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

db::UnixNanos at(std::uint64_t nanos) { return db::UnixNanos{db::UnixNanos::duration{nanos}}; }

constexpr std::int64_t kScale = db::kFixedPriceScale;

template <typename Msg>
void stamp(Msg& msg, db::RType rtype, std::uint32_t instrument_id, std::uint64_t ts) {
  msg.hd.length = static_cast<std::uint8_t>(sizeof(Msg) / db::RecordHeader::kLengthMultiplier);
  msg.hd.rtype = rtype;
  msg.hd.instrument_id = instrument_id;
  msg.hd.ts_event = at(ts);
}

db::InstrumentDefMsg definition(std::uint32_t id, const char* raw_symbol, db::InstrumentClass cls) {
  db::InstrumentDefMsg def{};
  stamp(def, db::RType::InstrumentDef, id, 1);
  std::strncpy(def.raw_symbol.data(), raw_symbol, def.raw_symbol.size() - 1);
  def.instrument_class = cls;
  return def;
}

template <typename Msg>
void feed(providers::DatabentoMapper& mapper, Msg& msg) {
  mapper.on_record(db::Record{&msg.hd});
}

TEST(Databento, ParentSymbolsCoverEveryRootOfAnIndex) {
  EXPECT_EQ(providers::databento_parent_symbols("SPX"),
            (std::vector<std::string>{"SPX.OPT", "SPXW.OPT"}));
  EXPECT_EQ(providers::databento_parent_symbols("SPY"), (std::vector<std::string>{"SPY.OPT"}));
}

TEST(Databento, MapsDefinitionsQuotesTradesAndOpenInterest) {
  Collector sink;
  providers::DatabentoMapper mapper(sink);

  auto put = definition(4242, "SPXW  261005P07405000", db::InstrumentClass::Put);
  feed(mapper, put);
  auto future = definition(7, "ESZ6", db::InstrumentClass::Future);  // not an option: ignored
  feed(mapper, future);

  db::CbboMsg quote{};
  stamp(quote, db::RType::Cbbo1S, 4242, 1'790'000'000'000'000'000);
  quote.levels[0].bid_px = 44 * kScale / 10;  // 4.40
  quote.levels[0].ask_px = 46 * kScale / 10;  // 4.60
  quote.levels[0].bid_sz = 314;
  quote.levels[0].ask_sz = 172;
  feed(mapper, quote);

  db::TradeMsg trade{};
  stamp(trade, db::RType::Mbp0, 4242, 1'790'000'001'000'000'000);
  trade.price = 45 * kScale / 10;
  trade.size = 3;
  feed(mapper, trade);

  db::StatMsg open_interest{};
  stamp(open_interest, db::RType::Statistics, 4242, 1'789'990'000'000'000'000);
  open_interest.stat_type = db::StatType::OpenInterest;
  open_interest.quantity = 1520;
  feed(mapper, open_interest);

  db::StatMsg other_stat = open_interest;
  other_stat.stat_type = db::StatType::SettlementPrice;
  feed(mapper, other_stat);

  const auto definitions = sink.all<md::ContractDefinition>();
  ASSERT_EQ(definitions.size(), 1u);
  EXPECT_EQ(definitions[0].id, 0u);
  EXPECT_EQ(definitions[0].contract.underlying, "SPX");
  EXPECT_DOUBLE_EQ(definitions[0].contract.strike, 7405.0);
  EXPECT_EQ(definitions[0].contract.type, pricing::OptionType::Put);

  const auto quotes = sink.all<md::OptionQuote>();
  ASSERT_EQ(quotes.size(), 1u);
  EXPECT_EQ(quotes[0].id, 0u);
  EXPECT_DOUBLE_EQ(quotes[0].bid, 4.4);
  EXPECT_DOUBLE_EQ(quotes[0].ask, 4.6);
  EXPECT_DOUBLE_EQ(quotes[0].bid_size, 314.0);
  EXPECT_EQ(quotes[0].ts, 1'790'000'000'000'000'000);

  const auto trades = sink.all<md::OptionTrade>();
  ASSERT_EQ(trades.size(), 1u);
  EXPECT_DOUBLE_EQ(trades[0].price, 4.5);
  EXPECT_DOUBLE_EQ(trades[0].size, 3.0);

  const auto oi = sink.all<md::OpenInterest>();
  ASSERT_EQ(oi.size(), 1u);
  EXPECT_DOUBLE_EQ(oi[0].contracts, 1520.0);
}

TEST(Databento, DropsRecordsForUndefinedInstrumentsAndHandlesEmptySides) {
  Collector sink;
  providers::DatabentoMapper mapper(sink);

  db::CbboMsg early{};
  stamp(early, db::RType::Cbbo1S, 99, 1);
  feed(mapper, early);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  EXPECT_EQ(mapper.undefined_records(), 1u);

  auto call = definition(99, "SPY   261016C00800000", db::InstrumentClass::Call);
  feed(mapper, call);
  db::CbboMsg one_sided{};
  stamp(one_sided, db::RType::Cbbo1S, 99, 2);
  one_sided.levels[0].bid_px = db::kUndefPrice;  // no bid
  one_sided.levels[0].ask_px = 5 * kScale / 100;
  feed(mapper, one_sided);
  const auto quotes = sink.all<md::OptionQuote>();
  ASSERT_EQ(quotes.size(), 1u);
  EXPECT_DOUBLE_EQ(quotes[0].bid, 0.0);
  EXPECT_DOUBLE_EQ(quotes[0].ask, 0.05);
}

}  // namespace

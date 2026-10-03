#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

#include "openport/md/recording.hpp"
#include "openport/providers/demo.hpp"
#include "openport/providers/scenario.hpp"
#include "openport/trading/types.hpp"

namespace {
using namespace openport;

std::filesystem::path temporary(const std::string& name) {
  const auto path = std::filesystem::temp_directory_path() / ("openport-demo-" + std::to_string(::getpid()) + "-" + name);
  std::filesystem::remove(path);
  return path;
}
bool on_tick(const std::string& root, double price) {
  const auto micros = trading::Money::from_double(price).micros();
  return micros % trading::tick_size(root, trading::Money::from_double(price)).micros() == 0;
}
struct Day {
  std::size_t events = 0;
  double checksum = 0;
  double volume_checksum = 0;
};
Day summary(const std::filesystem::path& path) {
  md::RecordingReader reader(path);
  Day day;
  for (auto event = reader.next(); event; event = reader.next()) {
    ++day.events;
    if (const auto* volume = std::get_if<md::OptionVolume>(&event->event))
      day.volume_checksum += volume->contracts * (volume->id + 1);
    if (const auto* q = std::get_if<md::OptionQuote>(&event->event)) day.checksum += q->bid + 2 * q->ask + q->bid_size;
    if (const auto* u = std::get_if<md::UnderlyingQuote>(&event->event)) day.checksum += u->last;
  }
  return day;
}

TEST(DemoMarket, SimulatesADayOfSpxSpyAndQqqChainsOnTheirOwnTicks) {
  const auto path = temporary("day");
  providers::write_demo_recording(path);
  md::RecordingReader reader(path);
  EXPECT_EQ(reader.header().provider, "demo");
  EXPECT_EQ(reader.header().subscription.underlyings, (std::vector<std::string>{"SPX", "SPY", "QQQ"}));
  const md::Date day{2026, 9, 16};
  const auto opening = md::new_york_to_utc(day, 9, 30);
  const auto closing = md::new_york_to_utc(day, 16, 0);
  EXPECT_EQ(reader.header().started, opening);

  std::map<md::InstrumentId, md::OptionContract> contracts;
  std::map<std::string, std::set<std::string>> expiries;
  std::map<std::string, md::Timestamp> last_print;
  std::map<std::string, md::UnderlyingClose> previous;
  md::Timestamp first = 0, last = 0;
  double low = 1e9, spx_close = 0;
  std::size_t quotes = 0, retired = 0, zero_bids = 0, open_interest = 0;
  for (auto event = reader.next(); event; event = reader.next()) {
    if (first == 0) first = event->received;
    ASSERT_GE(event->received, last);
    last = event->received;
    if (const auto* d = std::get_if<md::ContractDefinition>(&event->event)) {
      contracts[d->id] = d->contract;
      expiries[d->contract.root].insert(md::format_date(d->contract.expiry));
    } else if (const auto* oi = std::get_if<md::OpenInterest>(&event->event)) {
      EXPECT_GT(oi->contracts, 0);
      ++open_interest;
    } else if (const auto* close = std::get_if<md::UnderlyingClose>(&event->event)) {
      EXPECT_EQ(last_print.count(close->symbol), 0U);  // before the day's first print
      previous[close->symbol] = *close;
    } else if (const auto* u = std::get_if<md::UnderlyingQuote>(&event->event)) {
      last_print[u->symbol] = u->ts;
      if (u->symbol == "SPX") {
        low = std::min(low, u->last);
        spx_close = u->last;
      } else {
        EXPECT_NEAR(u->ask - u->bid, 0.01, 1e-9);
      }
    } else if (const auto* q = std::get_if<md::OptionQuote>(&event->event)) {
      const auto& c = contracts.at(q->id);
      if (q->ask == 0) {
        // Retired as it expires: SPXW at 16:00, SPY at 16:15.
        EXPECT_EQ(q->ts, c.expiry_time()) << c.osi_symbol();
        ++retired;
        continue;
      }
      ++quotes;
      ASSERT_LT(q->ts, c.expiry_time()) << c.osi_symbol();
      ASSERT_LT(q->bid, q->ask) << c.osi_symbol();
      ASSERT_TRUE(on_tick(c.root, q->ask)) << c.osi_symbol() << " ask " << q->ask;
      if (q->bid == 0) {
        ++zero_bids;
        EXPECT_EQ(q->bid_size, 0);
      } else {
        ASSERT_TRUE(on_tick(c.root, q->bid)) << c.osi_symbol() << " bid " << q->bid;
        EXPECT_GT(q->bid_size, 0);
      }
      EXPECT_GT(q->ask_size, 0);
    }
  }
  EXPECT_EQ(first, opening);
  EXPECT_EQ(last, md::new_york_to_utc(day, 16, 15));
  EXPECT_EQ(expiries["SPXW"], (std::set<std::string>{"2026-09-16", "2026-09-17", "2026-09-18", "2026-09-25"}));
  // September's AM monthly, listed through August, trades until the day before its third Friday.
  EXPECT_EQ(expiries["SPX"], (std::set<std::string>{"2026-09-18", "2026-10-16"}));
  EXPECT_EQ(expiries["SPY"], (std::set<std::string>{"2026-09-16", "2026-09-17", "2026-09-18", "2026-09-25", "2026-10-16"}));
  EXPECT_EQ(expiries["QQQ"], expiries["SPY"]);
  EXPECT_EQ(open_interest, contracts.size());
  // Each underlying's previous close, the level the day starts from.
  ASSERT_EQ(previous.size(), 3U);
  for (const auto& [symbol, price] : std::map<std::string, double>{{"SPX", 6000}, {"SPY", 598.80}, {"QQQ", 480}}) {
    EXPECT_EQ(previous[symbol].date, (md::Date{2026, 9, 15})) << symbol;
    EXPECT_DOUBLE_EQ(previous[symbol].price, price) << symbol;
  }
  // The index prints until the close and SPY trades on to 16:15.
  EXPECT_EQ(last_print["SPX"], closing);
  EXPECT_EQ(last_print["SPY"], md::new_york_to_utc(day, 16, 15));
  EXPECT_EQ(last_print["QQQ"], md::new_york_to_utc(day, 16, 15));
  EXPECT_GT(retired, 0);
  EXPECT_GT(zero_bids, 0);
  EXPECT_GT(quotes, 100'000);
  // A morning slide and an afternoon rally.
  EXPECT_LT(low, 6000 * 0.995);
  EXPECT_GT(spx_close, low * 1.005);
  std::filesystem::remove(path);
}

TEST(DemoMarket, IsTheSameDayEveryTimeAndOnlyOnTradingDays) {
  const auto a = temporary("a");
  const auto b = temporary("b");
  providers::write_demo_recording(a);
  providers::write_demo_recording(b);
  const auto first = summary(a);
  const auto second = summary(b);
  EXPECT_EQ(first.events, second.events);
  EXPECT_EQ(first.checksum, second.checksum);
  EXPECT_GT(first.volume_checksum, 0);
  EXPECT_EQ(first.volume_checksum, second.volume_checksum);
  // It never overwrites a file.
  EXPECT_ANY_THROW(providers::write_demo_recording(a));
  std::filesystem::remove(a);
  std::filesystem::remove(b);
  EXPECT_THROW(providers::write_demo_recording(temporary("weekend"), {providers::DemoDay::Reversal, md::Date{2026, 9, 19}, 1}),
               std::invalid_argument);
  EXPECT_THROW(providers::write_demo_recording(temporary("holiday"), {providers::DemoDay::Trend, md::Date{2026, 11, 26}, 1}),
               std::invalid_argument);
  EXPECT_TRUE(providers::simulated_provider("demo"));
  EXPECT_TRUE(providers::simulated_provider("replay (demo)"));
  EXPECT_FALSE(providers::simulated_provider("cboe"));
}

/// The SPX prints of one demo day: the first, lowest, highest and last.
struct Path {
  double first = 0, low = 1e9, high = 0, last = 0;
  std::size_t spx = 0, etf = 0, quotes = 0;
  md::Timestamp first_event = 0, last_event = 0;
};
Path walk(providers::DemoDay day) {
  const auto path = temporary("walk");
  providers::write_demo_recording(path, {day, std::nullopt, 0});
  md::RecordingReader reader(path);
  Path p;
  for (auto event = reader.next(); event; event = reader.next()) {
    if (p.first_event == 0) p.first_event = event->received;
    p.last_event = event->received;
    if (const auto* u = std::get_if<md::UnderlyingQuote>(&event->event)) {
      if (u->symbol != "SPX") { ++p.etf; continue; }
      if (p.spx++ == 0) p.first = u->last;
      p.low = std::min(p.low, u->last);
      p.high = std::max(p.high, u->last);
      p.last = u->last;
    } else if (std::holds_alternative<md::OptionQuote>(event->event)) {
      ++p.quotes;
    }
  }
  std::filesystem::remove(path);
  return p;
}

TEST(DemoMarket, EachDayFollowsItsScript) {
  ASSERT_EQ(providers::demo_days().size(), 5U);
  EXPECT_EQ(providers::demo_days().front().id, "reversal");
  ASSERT_NE(providers::find_demo_day("selloff"), nullptr);
  EXPECT_EQ(providers::find_demo_day("nope"), nullptr);

  const auto trend = walk(providers::DemoDay::Trend);
  EXPECT_GT(trend.last, trend.first * 1.005);
  const auto chop = walk(providers::DemoDay::Chop);
  EXPECT_LT(std::abs(chop.last / chop.first - 1), 0.004);
  EXPECT_LT(chop.high / chop.low - 1, 0.012);
  const auto selloff = walk(providers::DemoDay::Selloff);
  EXPECT_LT(selloff.low, selloff.first * 0.98);
  EXPECT_LT(selloff.last, selloff.first * 0.985);

  // Overnight only SPX options quote, from 20:15 the evening before to 09:25, and
  // the index prints once: its last close.
  const auto night = walk(providers::DemoDay::Overnight);
  const auto& info = *providers::find_demo_day("overnight");
  EXPECT_EQ(info.symbols, (std::vector<std::string>{"SPX"}));
  EXPECT_EQ(night.first_event, md::new_york_to_utc({2026, 9, 15}, 20, 15));
  EXPECT_EQ(night.last_event, md::new_york_to_utc({2026, 9, 16}, 9, 25));
  EXPECT_EQ(night.spx, 1U);
  EXPECT_EQ(night.etf, 0U);
  EXPECT_GT(night.quotes, 10'000U);
}
// Hash the ordered recording receipts and price/size payloads, independent of zstd framing.
std::uint64_t recording_hash(const std::filesystem::path& path, bool complete = false) {
  std::uint64_t hash = 14695981039346656037ULL;
  const auto mix = [&](std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) { hash ^= (value >> (8 * i)) & 255; hash *= 1099511628211ULL; }
  };
  const auto bits = [](double v) { std::uint64_t n = 0; std::memcpy(&n, &v, sizeof n); return n; };
  const auto text = [&](const std::string& value) {
    mix(value.size());
    for (const unsigned char byte : value) mix(byte);
  };
  md::RecordingReader reader(path);
  if (complete) {
    text(reader.header().provider);
    mix(reader.header().started);
    for (const auto& symbol : reader.header().subscription.underlyings) text(symbol);
  }
  while (const auto e = reader.next()) {
    if (!complete && std::holds_alternative<md::OptionVolume>(e->event)) continue;
    mix(static_cast<std::uint64_t>(e->received));
    mix(e->event.index());
    if (complete) {
      std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, md::ContractDefinition>) {
          mix(value.id);
          const auto& c = value.contract;
          text(c.osi_symbol()); text(c.underlying);
          mix(static_cast<unsigned>(c.style)); mix(static_cast<unsigned>(c.settlement));
          mix(bits(c.multiplier)); mix(c.standard);
        } else {
          mix(value.ts);
          if constexpr (std::is_same_v<T, md::OptionVolume>) { mix(value.id); mix(bits(value.contracts)); }
          else if constexpr (std::is_same_v<T, md::UnderlyingClose>) {
            text(value.symbol); mix(md::days_since_epoch(value.date)); mix(bits(value.price));
          } else if constexpr (std::is_same_v<T, md::UnderlyingQuote>) text(value.symbol);
          else if constexpr (std::is_same_v<T, md::SnapshotComplete>) text(value.underlying);
          else if constexpr (std::is_same_v<T, md::ProviderStatus>) {
            mix(static_cast<unsigned>(value.state)); text(value.message); text(value.underlying);
          }
        }
      }, e->event);
    }
    if (const auto* q = std::get_if<md::OptionQuote>(&e->event)) {
      mix(q->id); mix(static_cast<std::uint64_t>(q->ts));
      for (double v : {q->bid, q->ask, q->bid_size, q->ask_size}) mix(bits(v));
    } else if (const auto* spot = std::get_if<md::UnderlyingQuote>(&e->event)) {
      mix(static_cast<std::uint64_t>(spot->ts));
      for (double v : {spot->bid, spot->ask, spot->last}) mix(bits(v));
    } else if (const auto* oi = std::get_if<md::OpenInterest>(&e->event)) {
      mix(oi->id); mix(bits(oi->contracts));
    }
  }
  return hash;
}

TEST(DemoMarket, LegacyRecordingsAreUnchanged) {
  const std::map<std::string, std::uint64_t> expected{{"reversal", 18374653777601044835ULL},
      {"trend", 11930710970999195785ULL}, {"chop", 10781388607768009831ULL},
      {"selloff", 16480548078743839308ULL}, {"overnight", 11921129588866157280ULL}};
  for (const auto& [id, hash] : expected) {
    const auto& scenarios = providers::builtin_scenarios();
    const auto day = std::find_if(scenarios.begin(), scenarios.end(), [&](const auto& s) { return s.id == id; });
    ASSERT_NE(day, scenarios.end());
    const auto path = temporary("legacy");
    providers::write_scenario_recording(path, *day, day->date, day->seed, 1);
    EXPECT_EQ(recording_hash(path), hash) << id;
    std::filesystem::remove(path);
  }
}

TEST(DemoMarket, RevisionTwoRecordingIsUnchanged) {
  // Captured before revision 3; includes definitions, closes, volume and snapshot boundaries.
  const auto& scenarios = providers::builtin_scenarios();
  const auto day = std::find_if(scenarios.begin(), scenarios.end(), [](const auto& s) { return s.id == "overnight"; });
  ASSERT_NE(day, scenarios.end());
  const auto path = temporary("revision-two");
  providers::write_scenario_recording(path, *day, day->date, day->seed, 2);
  EXPECT_EQ(recording_hash(path, true), 2843919881680902969ULL);
  std::filesystem::remove(path);
}

TEST(DemoMarket, AHeldSeriesStaysListedUntilItsLastTrade) {
  // B11: the chain listed next month's third Friday only, so on the first of a month a
  // held monthly stopped being quoted two weeks before its last trade.
  const auto& scenarios = providers::builtin_scenarios();
  const auto day = std::find_if(scenarios.begin(), scenarios.end(), [](const auto& s) { return s.id == "quiet-grind"; });
  ASSERT_NE(day, scenarios.end());
  const auto dropped = [&](int revision) {
    std::size_t missing = 0;
    auto previous = providers::scenario_chain(*day, {2026, 7, 31}, revision);
    for (md::Date date{2026, 8, 3}; date <= md::Date{2027, 3, 31};
         date = md::trading_date(md::new_york_to_utc(date, 18, 0))) {
      auto chain = providers::scenario_chain(*day, date, revision);
      std::set<std::string> listed;
      for (const auto& c : chain) listed.insert(c.osi_symbol());
      for (const auto& c : previous)
        if (c.last_trade_time() >= md::new_york_to_utc(date, 9, 30) && !listed.contains(c.osi_symbol())) {
          EXPECT_EQ(revision, 1) << c.osi_symbol() << " is gone on " << md::format_date(date);
          ++missing;
        }
      previous = std::move(chain);
    }
    return missing;
  };
  EXPECT_GT(dropped(1), 0U);
  EXPECT_EQ(dropped(2), 0U);
  // The one from the report: October's monthly on 2026-10-01, and it is gone once expired.
  const auto listed = [&](md::Date date, std::string_view symbol) {
    const auto chain = providers::scenario_chain(*day, date);
    return std::any_of(chain.begin(), chain.end(), [&](const auto& c) { return c.osi_symbol() == symbol; });
  };
  const auto monthly = md::parse_osi("SPX261016C06000000")->osi_symbol();
  EXPECT_TRUE(listed({2026, 10, 1}, monthly));
  EXPECT_TRUE(listed({2026, 10, 15}, monthly));
  EXPECT_FALSE(listed({2026, 10, 16}, monthly));
  // Revision 2 lists revision 1's chain first, in its order.
  const auto first = providers::scenario_chain(*day, {2026, 10, 1}, 1);
  const auto second = providers::scenario_chain(*day, {2026, 10, 1});
  ASSERT_GT(second.size(), first.size());
  for (std::size_t i = 0; i < first.size(); ++i) EXPECT_EQ(second[i].osi_symbol(), first[i].osi_symbol());
  EXPECT_THROW((void)providers::scenario_chain(*day, {2026, 10, 1}, 4), std::invalid_argument);
}

TEST(DemoMarket, RevisionTwoKeepsEveryQuoteOfTheFirstAndAddsTheRest) {
  const auto& scenarios = providers::builtin_scenarios();
  const auto day = std::find_if(scenarios.begin(), scenarios.end(), [](const auto& s) { return s.id == "quiet-grind"; });
  ASSERT_NE(day, scenarios.end());
  struct Read {
    std::map<md::InstrumentId, std::string> symbols;
    std::map<std::string, std::vector<std::array<double, 5>>> quotes;
    std::size_t closes = 0;
  };
  const auto read = [&](int revision) {
    const auto path = temporary("revision-" + std::to_string(revision));
    providers::write_scenario_recording(path, *day, {2026, 10, 1}, day->seed, revision);
    md::RecordingReader reader(path);
    Read out;
    while (const auto event = reader.next()) {
      if (const auto* d = std::get_if<md::ContractDefinition>(&event->event)) out.symbols[d->id] = d->contract.osi_symbol();
      if (std::holds_alternative<md::UnderlyingClose>(event->event)) ++out.closes;
      if (const auto* q = std::get_if<md::OptionQuote>(&event->event))
        out.quotes[out.symbols.at(q->id)].push_back({static_cast<double>(q->ts), q->bid, q->ask, q->bid_size, q->ask_size});
    }
    std::filesystem::remove(path);
    return out;
  };
  const auto first = read(1);
  const auto second = read(2);
  EXPECT_EQ(first.closes, 0U);
  EXPECT_EQ(second.closes, 3U);
  for (const auto& [id, symbol] : first.symbols) {
    EXPECT_EQ(second.symbols.at(id), symbol);
    EXPECT_EQ(second.quotes.at(symbol), first.quotes.at(symbol)) << symbol;
  }
  const auto monthly = md::parse_osi("SPX261016C06000000")->osi_symbol();
  EXPECT_FALSE(first.quotes.contains(monthly));
  ASSERT_TRUE(second.quotes.contains(monthly));
  EXPECT_GT(second.quotes.at(monthly).size(), 1000U);
}

const providers::Scenario& index_scenario() {
  const auto& scenarios = providers::builtin_scenarios();
  return *std::find_if(scenarios.begin(), scenarios.end(), [](const auto& s) { return s.id == "index-spike"; });
}

TEST(DemoMarket, RevisionThreeListsTheIndexFamiliesAndVixCalendar) {
  const auto& scenario = index_scenario();
  const auto chain = providers::scenario_chain(scenario, scenario.date);
  std::set<std::string> roots;
  for (const auto& c : chain) {
    roots.insert(c.root);
    EXPECT_EQ(c.settlement, md::conventions_for_root(c.root).settlement);
    if (c.underlying == "VIX") {
      EXPECT_EQ(md::weekday(c.expiry), 3);
      EXPECT_GT(c.expiry, scenario.date);
    }
  }
  EXPECT_EQ(roots, (std::set<std::string>{"SPX", "SPXW", "SPY", "QQQ", "XSP", "NDX", "NDXP", "RUT", "RUTW", "VIX", "VIXW"}));
  EXPECT_LT(chain.size(), 5000U);  // Keep each snapshot bounded as the product set grows.
  const auto has = [&](md::Date date, const std::string& root, md::Date expiry) {
    const auto contracts = providers::scenario_chain(scenario, date);
    return std::any_of(contracts.begin(), contracts.end(), [&](const auto& c) { return c.root == root && c.expiry == expiry; });
  };
  EXPECT_TRUE(has({2026, 9, 15}, "VIX", {2026, 9, 16}));
  EXPECT_FALSE(has({2026, 9, 16}, "VIX", {2026, 9, 16}));
  EXPECT_TRUE(has({2026, 9, 17}, "VIX", {2026, 10, 21}));
  EXPECT_TRUE(has({2026, 9, 17}, "VIXW", {2026, 9, 23}));
  // June's Wednesday is Juneteenth; March's reference Friday is Good Friday.
  EXPECT_TRUE(has({2024, 6, 17}, "VIX", {2024, 6, 18}));
  EXPECT_FALSE(has({2024, 6, 17}, "VIX", {2024, 6, 19}));
  EXPECT_TRUE(has({2025, 3, 17}, "VIX", {2025, 3, 18}));
  // Rollover through December also computes the next year's monthly.
  EXPECT_TRUE(has({2026, 12, 31}, "VIX", {2027, 1, 20}));
}

TEST(DemoMarket, NewIndicesQuoteOnTheirTicksWithRelatedLevelsAndVixForwards) {
  const auto& scenario = index_scenario();
  const auto path = temporary("indices");
  providers::write_scenario_recording(path, scenario, scenario.date, scenario.seed);
  md::RecordingReader reader(path);
  EXPECT_EQ(reader.header().subscription.underlyings, scenario.symbols);
  std::map<md::InstrumentId, md::OptionContract> contracts;
  std::map<std::string, md::UnderlyingClose> previous;
  std::map<std::string, double> spot;
  std::map<md::Timestamp, double> vix;
  std::map<md::Date, std::map<pricing::OptionType, md::OptionQuote>> parity;
  std::set<std::string> quoted, below, above;
  const auto open = providers::scenario_open(scenario, scenario.date);
  while (const auto event = reader.next()) {
    if (const auto* d = std::get_if<md::ContractDefinition>(&event->event)) contracts[d->id] = d->contract;
    if (const auto* close = std::get_if<md::UnderlyingClose>(&event->event)) previous[close->symbol] = *close;
    if (const auto* u = std::get_if<md::UnderlyingQuote>(&event->event)) {
      spot[u->symbol] = u->last;
      if (u->symbol == "VIX") vix[u->ts] = u->last;
      if (u->symbol == "SPX") { EXPECT_NEAR(spot["XSP"], u->last / 10, 0.006); }
      if (u->symbol == "QQQ" && u->ts <= md::new_york_to_utc(scenario.date, 16, 0)) {
        EXPECT_NEAR(spot["NDX"] / 21000, u->last / 480, 0.000011);
      }
    }
    if (const auto* q = std::get_if<md::OptionQuote>(&event->event); q && q->ask > 0) {
      const auto& c = contracts.at(q->id);
      ASSERT_TRUE(on_tick(c.root, q->bid)) << c.osi_symbol() << " bid " << q->bid;
      ASSERT_TRUE(on_tick(c.root, q->ask)) << c.osi_symbol() << " ask " << q->ask;
      ASSERT_GT(q->ask, q->bid);
      quoted.insert(c.root);
      (q->ask < 3 ? below : above).insert(c.underlying);
      if (q->ts == open && c.underlying == "VIX" && c.strike == 17) parity[c.expiry][c.type] = *q;
    }
  }
  EXPECT_EQ(quoted.size(), 11U);
  EXPECT_EQ(below.size(), 7U);
  EXPECT_EQ(above.size(), 7U);
  EXPECT_EQ(previous.size(), 7U);
  EXPECT_DOUBLE_EQ(previous["XSP"].price, 600);
  EXPECT_DOUBLE_EQ(previous["NDX"].price, 21000);
  EXPECT_DOUBLE_EQ(previous["RUT"].price, 2300);
  EXPECT_DOUBLE_EQ(previous["VIX"].price, 15.4);
  EXPECT_GT(vix[md::new_york_to_utc(scenario.date, 12, 30)], vix[md::new_york_to_utc(scenario.date, 12, 29, 45)] + 0.7);
  EXPECT_LT(vix[md::new_york_to_utc(scenario.date, 14, 45)], vix[md::new_york_to_utc(scenario.date, 14, 44, 45)] - 0.3);
  ASSERT_GE(parity.size(), 3U);
  double prior = 0;
  for (const auto& [date, pair] : parity) {
    ASSERT_EQ(pair.size(), 2U);
    const auto& call = pair.at(pricing::OptionType::Call);
    const auto& put = pair.at(pricing::OptionType::Put);
    const double years = md::years_between(open, md::new_york_to_utc(date, 9, 30));
    const double forward = 17 + ((call.bid + call.ask) - (put.bid + put.ask)) / 2 * std::exp(0.04 * years);
    const double expected = 19.5 + (vix.at(open) - 19.5) * std::exp(-4 * years);
    EXPECT_NEAR(forward, expected, 0.03);
    EXPECT_GT(forward, prior);
    prior = forward;
  }
  std::filesystem::remove(path);
}

TEST(DemoMarket, AmMonthliesAndSameDatePmSeriesHaveSeparateLastTradingDays) {
  const auto& scenario = index_scenario();
  const md::Date expiry{2026, 9, 18};
  const auto before = providers::scenario_chain(scenario, {2026, 9, 17});
  const auto after = providers::scenario_chain(scenario, expiry);
  for (const auto* underlying : {"SPX", "NDX", "RUT", "XSP"}) {
    const auto count = [&](const auto& chain, md::Settlement settlement) {
      return std::count_if(chain.begin(), chain.end(), [&](const auto& c) {
        return c.underlying == underlying && c.expiry == expiry && c.settlement == settlement;
      });
    };
    if (std::string_view(underlying) != "XSP") { EXPECT_GT(count(before, md::Settlement::AM), 0); }
    else { EXPECT_EQ(count(before, md::Settlement::AM), 0); }
    EXPECT_EQ(count(after, md::Settlement::AM), 0);
    EXPECT_GT(count(before, md::Settlement::PM), 0);
    EXPECT_GT(count(after, md::Settlement::PM), 0);
  }
  // Held series stay listed across month boundaries, holidays and the turn of the year.
  auto previous = providers::scenario_chain(scenario, {2026, 12, 1});
  for (auto date = md::Date{2026, 12, 2}; date <= md::Date{2027, 1, 22};
       date = md::trading_date(md::new_york_to_utc(date, 18, 0))) {
    const auto chain = providers::scenario_chain(scenario, date);
    std::set<std::string> symbols;
    for (const auto& c : chain) symbols.insert(c.osi_symbol());
    for (const auto& c : previous) {
      if (c.last_trade_time() > md::new_york_to_utc(date, 9, 30)) {
        EXPECT_TRUE(symbols.contains(c.osi_symbol())) << c.osi_symbol() << " on " << md::format_date(date);
      }
    }
    previous = chain;
  }
}

TEST(DemoMarket, AmQuotesEndAtTheirLastRegularCloseBeforeCurbAndOvernight) {
  for (const auto& [date, root] : std::vector<std::pair<md::Date, std::string>>{
      {{2026, 9, 17}, "SPX"}, {{2026, 9, 15}, "VIX"}, {{2026, 9, 22}, "VIXW"}, {{2024, 6, 17}, "VIX"}}) {
    auto scenario = index_scenario();
    scenario.date = date;
    if (root != "SPX") scenario.symbols = {"VIX"};
    scenario.sessions.clear();
    for (const auto* kind : {"regular", "curb", "overnight", "regular"})
      scenario.sessions.push_back({kind, {{1, 0}}, 0, 0, 0, {}, {}});
    const auto windows = providers::scenario_windows(scenario, date);
    const auto cutoff = windows[0].last;
    const auto expiry = windows[3].date;
    const auto path = temporary("am-close");
    providers::write_scenario_recording(path, scenario, date, scenario.seed);
    md::RecordingReader reader(path);
    std::map<md::InstrumentId, md::OptionContract> contracts;
    std::set<std::string> last_day, retired, settlement_day;
    std::map<md::InstrumentId, double> asks;
    while (const auto event = reader.next()) {
      if (const auto* d = std::get_if<md::ContractDefinition>(&event->event)) contracts[d->id] = d->contract;
      if (const auto* q = std::get_if<md::OptionQuote>(&event->event)) {
        asks[q->id] = q->ask;
        const auto& c = contracts.at(q->id);
        if (q->ask > 0) {
          ASSERT_LT(q->ts, c.last_trade_time()) << c.osi_symbol();
          if (q->ts >= windows[3].first && c.expiry == expiry) settlement_day.insert(c.root);
        } else if (c.expiry == expiry && c.settlement == md::Settlement::AM) {
          EXPECT_EQ(q->ts, cutoff) << c.osi_symbol();
          retired.insert(c.root);
        }
      }
      if (const auto* snapshot = std::get_if<md::SnapshotComplete>(&event->event);
          snapshot && snapshot->ts == cutoff - windows[0].step) {
        // Prices can stay unchanged in the last interval; their live quotes remain.
        for (const auto& [id, c] : contracts)
          if (c.underlying == snapshot->underlying && c.expiry == expiry && asks[id] > 0) last_day.insert(c.root);
      }
    }
    EXPECT_TRUE(last_day.contains(root)) << root;
    EXPECT_TRUE(retired.contains(root)) << root;
    EXPECT_FALSE(settlement_day.contains(root)) << root;
    if (root == "SPX") {
      EXPECT_EQ(retired, (std::set<std::string>{"SPX", "NDX", "RUT"}));
      for (const auto* pm : {"SPXW", "XSP", "NDXP", "RUTW"}) {
        EXPECT_TRUE(last_day.contains(pm)) << pm;
        EXPECT_TRUE(settlement_day.contains(pm)) << pm;
      }
    }
    std::filesystem::remove(path);
  }
}

TEST(DemoMarket, SimulatedVolumeRisesAndFavoursNearMoneyAndFrontExpiry) {
  const auto path = temporary("volume");
  providers::write_demo_recording(path);
  md::RecordingReader reader(path);
  std::map<md::InstrumentId, md::OptionContract> definitions;
  std::map<md::InstrumentId, double> latest;
  while (const auto event = reader.next()) {
    if (const auto* definition = std::get_if<md::ContractDefinition>(&event->event)) {
      definitions[definition->id] = definition->contract;
    } else if (const auto* volume = std::get_if<md::OptionVolume>(&event->event)) {
      ASSERT_GE(volume->contracts, latest[volume->id]);
      EXPECT_EQ(volume->contracts, std::floor(volume->contracts));
      latest[volume->id] = volume->contracts;
    }
  }
  double near = 0, far = 0, back = 0;
  for (const auto& [id, contract] : definitions) {
    if (contract.underlying != "SPX" || contract.type != pricing::OptionType::Call) continue;
    if (contract.expiry == md::Date{2026, 9, 16}) {
      if (contract.strike == 6000) near = latest[id];
      if (contract.strike == 6180) far = latest[id];
    } else if (contract.expiry == md::Date{2026, 9, 25} && contract.strike == 6000) {
      back = latest[id];
    }
  }
  EXPECT_GT(near, far);
  EXPECT_GT(near, back);
  EXPECT_GT(back, 0);
  EXPECT_EQ(latest.size(), definitions.size());
  std::filesystem::remove(path);
}

}  // namespace

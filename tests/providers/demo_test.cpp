#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

#include "openport/md/recording.hpp"
#include "openport/providers/demo.hpp"
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
};
Day summary(const std::filesystem::path& path) {
  md::RecordingReader reader(path);
  Day day;
  for (auto event = reader.next(); event; event = reader.next()) {
    ++day.events;
    if (const auto* q = std::get_if<md::OptionQuote>(&event->event)) day.checksum += q->bid + 2 * q->ask + q->bid_size;
    if (const auto* u = std::get_if<md::UnderlyingQuote>(&event->event)) day.checksum += u->last;
  }
  return day;
}

TEST(DemoMarket, SimulatesADayOfSpxAndSpyChainsOnTheirOwnTicks) {
  const auto path = temporary("day");
  providers::write_demo_recording(path);
  md::RecordingReader reader(path);
  EXPECT_EQ(reader.header().provider, "demo");
  EXPECT_EQ(reader.header().subscription.underlyings, (std::vector<std::string>{"SPX", "SPY"}));
  const md::Date day{2026, 9, 16};
  const auto opening = md::new_york_to_utc(day, 9, 30);
  const auto closing = md::new_york_to_utc(day, 16, 0);
  EXPECT_EQ(reader.header().started, opening);

  std::map<md::InstrumentId, md::OptionContract> contracts;
  std::map<std::string, std::set<std::string>> expiries;
  std::map<std::string, md::Timestamp> last_print;
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
  EXPECT_EQ(expiries["SPX"], (std::set<std::string>{"2026-10-16"}));
  EXPECT_EQ(expiries["SPY"], (std::set<std::string>{"2026-09-16", "2026-09-17", "2026-09-18", "2026-09-25", "2026-10-16"}));
  EXPECT_EQ(open_interest, contracts.size());
  // The index prints until the close and SPY trades on to 16:15.
  EXPECT_EQ(last_print["SPX"], closing);
  EXPECT_EQ(last_print["SPY"], md::new_york_to_utc(day, 16, 15));
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
  // It never overwrites a file.
  EXPECT_ANY_THROW(providers::write_demo_recording(a));
  std::filesystem::remove(a);
  std::filesystem::remove(b);
  EXPECT_THROW(providers::write_demo_recording(temporary("weekend"), {{2026, 9, 19}, 1}), std::invalid_argument);
  EXPECT_THROW(providers::write_demo_recording(temporary("holiday"), {{2026, 11, 26}, 1}), std::invalid_argument);
  EXPECT_TRUE(providers::simulated_provider("demo"));
  EXPECT_TRUE(providers::simulated_provider("replay (demo)"));
  EXPECT_FALSE(providers::simulated_provider("cboe"));
}
}  // namespace

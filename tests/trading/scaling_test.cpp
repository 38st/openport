#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <string>

#include "support/scripted_market.hpp"

namespace {
using namespace openport;
using trading::Side;

trading::SessionConfig configuration() {
  trading::SessionConfig config;
  config.initial_cash = trading::Money::parse("1000000000");
  // Each round trip pays the spread and fees; the history must not stop at the loss limit.
  config.limits.max_daily_loss = trading::Money::parse("1000000000");
  return config;
}

// Both books exist before timing starts, so setup cannot separate their samples
// into different periods of machine load.
struct QuoteBook {
  test::ScriptedMarket market;
  trading::TradingSession session{configuration(), market.time};

  explicit QuoteBook(std::int64_t fills) {
    market.seed(session, "4.00", "4.20", fills + 10);
    for (std::int64_t i = 0; i < fills; ++i) {
      const auto side = i % 2 == 0 ? Side::Buy : Side::Sell;
      EXPECT_TRUE(session.submit(market.market(std::to_string(i), 1, side), market.time).decision.ok());
    }
    // A bid inside the price band, below the ask: every batch takes the transaction path.
    EXPECT_TRUE(session.submit(market.limit("resting", 1, "3.70"), market.time).decision.ok());
    EXPECT_TRUE(session.snapshot()->positions.empty());
    EXPECT_EQ(session.snapshot()->recent_fills.size(), static_cast<std::size_t>(fills));
  }

  void hold() {
    EXPECT_TRUE(session.submit(market.market("held", 1), market.time).decision.ok());
    EXPECT_EQ(session.snapshot()->positions.size(), 1U);
  }

  double per_batch() {
    // This fixture is synchronous and starts no worker threads. Process CPU time
    // measures the entire transaction, including history copies and reviews, but
    // excludes time the scheduler gives to other builds/tests. A review counter
    // alone would miss a regression in copying or rule evaluation.
    const auto start = std::clock();
    for (int i = 0; i < 100; ++i) {
      ++market.observation;
      EXPECT_TRUE(session.on_quotes({market.quote()}, {market.valuation()}, market.time).decision.ok());
    }
    const auto end = std::clock();
    EXPECT_NE(start, static_cast<std::clock_t>(-1));
    EXPECT_NE(end, static_cast<std::clock_t>(-1));
    return static_cast<double>(end - start) / CLOCKS_PER_SEC / 100;
  }
};

void compare(QuoteBook& small, QuoteBook& large, const char* phase) {
  SCOPED_TRACE(phase);
  double small_best = std::numeric_limits<double>::infinity();
  double large_best = std::numeric_limits<double>::infinity();
  // Interleave rounds and alternate their order to share cache/frequency/load
  // conditions. Minima discard transient stalls without relaxing the 4x bound.
  for (int round = 0; round < 15; ++round) {
    const auto measure_small = [&] { small_best = std::min(small_best, small.per_batch()); };
    const auto measure_large = [&] { large_best = std::min(large_best, large.per_batch()); };
    if (round % 2 == 0) { measure_small(); measure_large(); }
    else { measure_large(); measure_small(); }
  }
  ASSERT_GT(small_best, 0);
  EXPECT_LT(large_best, 4 * small_best)
      << phase << ": " << small_best * 1e6 << " CPU us after 10 fills, "
      << large_best * 1e6 << " CPU us after 10,000";
}

// Quote handling costs the open positions and working orders, not the account's
// history: a transaction's copy of the account shares the history, the rules read
// the positions, and reviews sample only trades that can still change. Before,
// a batch after 10,000 fills cost some 270 times one after 10 (BM_TradingOnQuotes).
TEST(TradingScaling, QuoteBatchesCostTheOpenBookNotTheHistory) {
  if (std::getenv("OPENPORT_VERIFY_REVIEWS") || std::getenv("OPENPORT_VERIFY_JOURNAL"))
    GTEST_SKIP() << "The rebuilds these checks compare against cost the whole history by design";
  QuoteBook small(10), large(10000);
  compare(small, large, "flat");
  small.hold();
  large.hold();
  compare(small, large, "holding");
}
}  // namespace

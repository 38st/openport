#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

#include "support/scripted_market.hpp"

namespace {
using namespace openport;
using trading::Side;

struct Batches {
  double flat = 0;     ///< Seconds per quote batch with only a resting order.
  double holding = 0;  ///< The same with a position open, whose trade review samples.
};
/// Quote batches after `fills` alternating one-lot market fills, as
/// BM_TradingOnQuotes builds them, with a resting bid so every batch takes the
/// whole transaction path: flat, then holding one contract.
Batches quote_batches(std::int64_t fills) {
  test::ScriptedMarket market;
  trading::SessionConfig config;
  config.initial_cash = trading::Money::parse("1000000000");
  // Each round trip pays the spread and fees; the history must not stop at the loss limit.
  config.limits.max_daily_loss = trading::Money::parse("1000000000");
  trading::TradingSession session(config, market.time);
  market.seed(session, "4.00", "4.20", fills + 10);
  for (std::int64_t i = 0; i < fills; ++i) {
    const auto side = i % 2 == 0 ? Side::Buy : Side::Sell;
    EXPECT_TRUE(session.submit(market.market(std::to_string(i), 1, side), market.time).decision.ok());
  }
  // A bid inside the price band, below the ask: it rests.
  EXPECT_TRUE(session.submit(market.limit("resting", 1, "3.70"), market.time).decision.ok());
  // The fastest of several rounds: noise only ever adds time.
  const auto per_batch = [&] {
    double best = 1e9;
    for (int round = 0; round < 7; ++round) {
      const auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < 100; ++i) {
        ++market.observation;
        EXPECT_TRUE(session.on_quotes({market.quote()}, {market.valuation()}, market.time).decision.ok());
      }
      best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() / 100);
    }
    return best;
  };
  Batches out;
  out.flat = per_batch();
  EXPECT_TRUE(session.submit(market.market("held", 1), market.time).decision.ok());
  EXPECT_FALSE(session.snapshot()->positions.empty());
  out.holding = per_batch();
  return out;
}

// Quote handling costs the open positions and working orders, not the account's
// history: a transaction's copy of the account shares the history, the rules read
// the positions, and reviews sample only trades that can still change. Before,
// a batch after 10,000 fills cost some 270 times one after 10 (BM_TradingOnQuotes).
TEST(TradingScaling, QuoteBatchesCostTheOpenBookNotTheHistory) {
  if (std::getenv("OPENPORT_VERIFY_REVIEWS") || std::getenv("OPENPORT_VERIFY_JOURNAL"))
    GTEST_SKIP() << "The rebuilds these checks compare against cost the whole history by design";
  const auto small = quote_batches(10);
  const auto large = quote_batches(10000);
  EXPECT_LT(large.flat, 4 * small.flat)
      << "flat: " << small.flat * 1e6 << " us after 10 fills, " << large.flat * 1e6 << " us after 10,000";
  EXPECT_LT(large.holding, 4 * small.holding)
      << "holding: " << small.holding * 1e6 << " us after 10 fills, " << large.holding * 1e6 << " us after 10,000";
}
}  // namespace

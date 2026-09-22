#include "openport/analytics/chain_analytics.hpp"

#include <gtest/gtest.h>

#include <cmath>

#include "openport/analytics/chain_book.hpp"
#include "openport/pricing/black.hpp"
#include "support/synthetic_chain.hpp"

namespace {

using namespace openport;
using analytics::ChainBook;
using pricing::OptionType;

using test::SyntheticChain;
constexpr double kSpot = SyntheticChain::kSpot;

double smile(double strike, double forward) { return SyntheticChain::smile(strike, forward); }

TEST(ChainAnalytics, RecoversTheForwardAndTheSmile) {
  SyntheticChain chain;
  const auto& underlying = chain.book.underlyings().at("SPX");
  const auto metrics = analytics::analyze(underlying, chain.book, chain.as_of);
  ASSERT_EQ(metrics.slices.size(), 1u);
  const auto& slice = metrics.slices[0];

  EXPECT_NEAR(slice.forward.forward, chain.forward, 1e-6 * chain.forward);
  EXPECT_NEAR(slice.forward.discount, chain.discount, 1e-8);
  EXPECT_EQ(metrics.options_priced, 2 * 41);

  for (const auto& row : slice.strikes) {
    // Quotes are symmetric around fair value, so the mid is exact and so is its IV.
    EXPECT_NEAR(row.iv, smile(row.strike, chain.forward), 1e-6) << row.strike;
  }
  EXPECT_NEAR(slice.atm_iv, smile(chain.forward, chain.forward), 2e-3);
}

TEST(ChainAnalytics, GreeksAreConsistentAcrossCallAndPut) {
  SyntheticChain chain;
  const auto metrics =
      analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  for (const auto& row : metrics.slices[0].strikes) {
    // Same smile vol on both sides: put-call parity for delta, identical gamma and vega.
    const double carry = chain.forward / kSpot;
    EXPECT_NEAR(row.call.delta - row.put.delta, chain.discount * carry, 1e-9) << row.strike;
    EXPECT_NEAR(row.call.gamma, row.put.gamma, 1e-12);
    EXPECT_NEAR(row.call.vega, row.put.vega, 1e-9);
    EXPECT_GT(row.call.gamma, 0.0);
    EXPECT_LT(row.call.theta, 0.0);
  }
}

TEST(ChainAnalytics, ExposureFollowsTheOpenInterestConvention) {
  SyntheticChain chain;
  const auto metrics =
      analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  const auto& exposure = metrics.exposure;

  // Calls dominate above spot (positive GEX), puts below (negative GEX).
  for (const auto& row : metrics.slices[0].strikes) {
    if (row.strike > kSpot + 1) EXPECT_GT(row.gex, 0.0) << row.strike;
    if (row.strike < kSpot - 1) EXPECT_LT(row.gex, 0.0) << row.strike;
  }
  EXPECT_GT(exposure.call_wall, kSpot);
  EXPECT_LE(exposure.put_wall, kSpot);  // the at-the-money strike carries heavy put OI here
  ASSERT_TRUE(std::isfinite(exposure.gamma_flip));
  EXPECT_NEAR(exposure.gamma_flip, kSpot, 0.05 * kSpot);

  // One strike by hand: gamma * OI * 100 * S^2 * 1%.
  const auto& row = metrics.slices[0].strikes[30];
  const double expected = (row.call.gamma * row.call.open_interest -
                           row.put.gamma * row.put.open_interest) *
                          100.0 * kSpot * kSpot * 0.01;
  EXPECT_NEAR(row.gex, expected, 1e-6 * std::abs(expected));
}

TEST(ChainAnalytics, SkipsExpiredSlices) {
  SyntheticChain chain;
  const md::Timestamp after_expiry = md::new_york_to_utc(md::Date{2026, 10, 23}, 10, 0);
  const auto metrics =
      analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, after_expiry);
  EXPECT_TRUE(metrics.slices.empty());
}

TEST(ChainBook, KeepsAmAndPmSettledExpiriesApart) {
  ChainBook book;
  book.apply(md::ContractDefinition{0, *md::parse_osi("SPX261016C05000000")});
  book.apply(md::ContractDefinition{1, *md::parse_osi("SPXW261016C05000000")});
  book.apply(md::OptionQuote{7, 1, 1.0, 2.0, 1, 1});  // undefined id: ignored
  const auto& spx = book.underlyings().at("SPX");
  EXPECT_EQ(spx.expiries.size(), 2u);
  EXPECT_EQ(book.contracts(), 2u);
  EXPECT_EQ(book.option(7), nullptr);
}

}  // namespace

#include "openport/pricing/binomial.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace {

using openport::pricing::binomial_price;
using openport::pricing::bsm_price;
using openport::pricing::BsmInputs;
using openport::pricing::CashDividend;
using openport::pricing::binomial_early_exercise_premium;
using openport::pricing::ExerciseStyle;
using openport::pricing::OptionType;
using openport::pricing::TreeMethod;

constexpr auto kEuropean = ExerciseStyle::European;
constexpr auto kAmerican = ExerciseStyle::American;
constexpr auto kCrr = TreeMethod::CoxRossRubinstein;
constexpr auto kLr = TreeMethod::LeisenReimer;

TEST(Binomial, EuropeanTreesConvergeToBlackScholes) {
  for (OptionType type : {OptionType::Call, OptionType::Put}) {
    for (double strike : {80.0, 100.0, 120.0}) {
      const BsmInputs in{type, 100.0, strike, 0.75, 0.04, 0.01, 0.3};
      const double exact = bsm_price(in);
      EXPECT_NEAR(binomial_price(in, kEuropean, kLr, 1001), exact, 1e-6);
      EXPECT_NEAR(binomial_price(in, kEuropean, kCrr, 2001), exact, 5e-3);
    }
  }
}

TEST(Binomial, LeisenReimerConvergesMuchFasterThanCrr) {
  const BsmInputs in{OptionType::Put, 100.0, 105.0, 1.0, 0.05, 0.0, 0.25};
  const double exact = bsm_price(in);
  const double lr_error = std::abs(binomial_price(in, kEuropean, kLr, 101) - exact);
  const double crr_error = std::abs(binomial_price(in, kEuropean, kCrr, 101) - exact);
  EXPECT_LT(lr_error * 20, crr_error);
}

TEST(Binomial, AmericanCallWithoutDividendsIsNeverExercisedEarly) {
  // With r >= 0 and no dividends a call is worth more alive than exercised, so
  // the American and European trees must agree node for node.
  const BsmInputs in{OptionType::Call, 100.0, 95.0, 1.0, 0.05, 0.0, 0.3};
  for (TreeMethod method : {kCrr, kLr}) {
    EXPECT_NEAR(binomial_price(in, kAmerican, method, 401),
                binomial_price(in, kEuropean, method, 401), 1e-12);
  }
}

TEST(Binomial, AmericanPutMatchesQuantLib) {
  // S=36, K=40, r=6%, vol=20%, T=1: the classic case from Longstaff and Schwartz (2001).
  const BsmInputs in{OptionType::Put, 36.0, 40.0, 1.0, 0.06, 0.0, 0.2};
  const double tree = binomial_price(in, kAmerican, kLr, 1001);

  // Same tree, same steps: QuantLib's BinomialVanillaEngine("lr", 1001) gives 4.48618803.
  EXPECT_NEAR(tree, 4.48618803, 1e-8);
  // The converged value is ~4.48667 (QuantLib LR at 20001 steps, FD at 8000x8000).
  // An American tree converges like 1/n, not 1/n^2, because of the exercise boundary.
  EXPECT_NEAR(tree, 4.48667, 1e-3);
  EXPECT_NEAR(bsm_price(in), 3.84431, 1e-5);  // the early-exercise premium is ~0.64
}

TEST(Binomial, AmericanCallIsExercisedEarlyAheadOfAHighDividendYield) {
  const BsmInputs in{OptionType::Call, 100.0, 80.0, 1.0, 0.01, 0.08, 0.2};
  EXPECT_GT(binomial_price(in, kAmerican, kLr, 501), binomial_price(in, kEuropean, kLr, 501) + 0.1);
}

TEST(Binomial, LeisenReimerRoundsStepsUpToOdd) {
  const BsmInputs in{OptionType::Put, 100.0, 100.0, 0.5, 0.03, 0.0, 0.2};
  EXPECT_DOUBLE_EQ(binomial_price(in, kAmerican, kLr, 100),
                   binomial_price(in, kAmerican, kLr, 101));
}

TEST(Binomial, ZeroVolAmericanRespectsImmediateExercise) {
  const BsmInputs in{OptionType::Put, 80, 100, 1, 0.1, 0, 0};
  for (auto method : {kCrr, kLr})
    EXPECT_NEAR(binomial_price(in, kAmerican, method, 101), 20, 1e-12);
}

TEST(Binomial, InadmissibleCrrProbabilitiesHaveAStableFallback) {
  for (auto type : {OptionType::Put, OptionType::Call}) {
    const BsmInputs in{type, 100, 100, 1, 0.1, 0, 0.01};
    const double p = binomial_price(in, kEuropean, kCrr, 1);
    EXPECT_TRUE(std::isfinite(p));
    EXPECT_GE(p, 0);
    EXPECT_NEAR(p, bsm_price(in), 1e-6);
  }
}

TEST(Binomial, SaturatedLeisenReimerTailsStayFinite) {
  for (auto type : {OptionType::Put, OptionType::Call}) {
    for (double s : {1.0, 10000.0}) {
      const BsmInputs in{type, s, 100, 1, 0.05, 0, 0.01};
      const double p = binomial_price(in, kEuropean, kLr, 101);
      EXPECT_TRUE(std::isfinite(p));
      EXPECT_GE(p, 0);
      EXPECT_NEAR(p, bsm_price(in), 1e-7);
      EXPECT_GE(binomial_price(in, kAmerican, kLr, 101),
                std::max(openport::pricing::omega(type) * (s - 100), 0.0));
    }
  }
}

TEST(Binomial, DeterministicAmericanCanExerciseBetweenTodayAndExpiry) {
  const BsmInputs in{OptionType::Call, 100, 90, 10, .2, .1, 0};
  const double optimal_time = std::log(.1 * 100 / (.2 * 90)) / (.1 - .2);
  const double exact = 100 * std::exp(-.1 * optimal_time) - 90 * std::exp(-.2 * optimal_time);
  EXPECT_GT(exact, std::max(10.0, bsm_price(in)));
  for (auto method : {kCrr, kLr})
    EXPECT_NEAR(binomial_price(in, kAmerican, method, 101), exact, 1e-10);
}

TEST(Binomial, EmptyOrOutOfTermCashScheduleLeavesPricesExactlyUnchanged) {
  const std::vector<CashDividend> ignored{{-.1, 5}, {0, 5}, {.75, 5}, {1, 5}, {.2, 0}};
  for (auto type : {OptionType::Call, OptionType::Put}) {
    const BsmInputs in{type, 100, 95, .75, .04, .01, .3};
    for (auto style : {kAmerican, kEuropean}) {
      for (auto method : {kCrr, kLr}) {
        const double original = binomial_price(in, style, method, 101);
        EXPECT_DOUBLE_EQ(binomial_price(in, style, method, 101, {}), original);
        EXPECT_DOUBLE_EQ(binomial_price(in, style, method, 101, ignored), original);
      }
    }
    EXPECT_DOUBLE_EQ(binomial_early_exercise_premium(in, 31, ignored),
                     binomial_early_exercise_premium(in, 31));
  }
}

TEST(Binomial, CashDividendEuropeanMatchesBlackScholesOnEscrowedSpot) {
  const std::vector<CashDividend> cash{{.6, 1.5}, {.2, 2}};
  for (auto type : {OptionType::Call, OptionType::Put}) {
    for (double yield : {-.01, .02}) {
      const BsmInputs in{type, 100, 95, .75, .04, yield, .3};
      auto escrowed = in;
      for (const auto& d : cash) escrowed.spot -= d.amount * std::exp(-in.rate * d.time);
      EXPECT_NEAR(binomial_price(in, kEuropean, kLr, 1001, cash), bsm_price(escrowed), 2e-6);
      EXPECT_NEAR(binomial_price(in, kEuropean, kCrr, 2001, cash), bsm_price(escrowed), .005);
    }
  }
}

TEST(Binomial, CashDividendAmericanConvergesAndPremiumUsesTheSameLattice) {
  const std::vector<CashDividend> cash{{.19, 3}, {.43, 2}};
  for (auto type : {OptionType::Call, OptionType::Put}) {
    const BsmInputs in{type, 100, 95, .75, .04, .01, .25};
    for (auto method : {kCrr, kLr}) {
      const double reference = binomial_price(in, kAmerican, method, 2001, cash);
      const double coarse = binomial_price(in, kAmerican, method, 31, cash);
      const double fine = binomial_price(in, kAmerican, method, 501, cash);
      EXPECT_LT(std::abs(fine - reference), std::abs(coarse - reference));
      EXPECT_NEAR(fine, reference, .015);
    }
    EXPECT_NEAR(binomial_early_exercise_premium(in, 31, cash),
                binomial_price(in, kAmerican, kLr, 31, cash) -
                    binomial_price(in, kEuropean, kLr, 31, cash), 1e-12);
  }
}

TEST(Binomial, DeepCallJustBeforeLargeCashDividendExercisesImmediately) {
  const std::vector<CashDividend> cash{{1e-6, 10}};
  const BsmInputs in{OptionType::Call, 100, 60, .5, .04, 0, .15};
  for (auto method : {kCrr, kLr}) {
    EXPECT_NEAR(binomial_price(in, kAmerican, method, 501, cash), 40, 1e-10);
    EXPECT_LT(binomial_price(in, kEuropean, method, 501, cash), 32);
  }
  EXPECT_GT(binomial_early_exercise_premium(in, 31, cash), 8);
}

TEST(Binomial, DeterministicCashExerciseChecksBothSidesOfJumps) {
  const std::vector<CashDividend> cash{{.4, 10}};
  for (auto method : {kCrr, kLr}) {
    const BsmInputs call{OptionType::Call, 100, 80, 1, .05, 0, 0};
    const double before_ex = 100 - 80 * std::exp(-.05 * .4);
    EXPECT_NEAR(binomial_price(call, kAmerican, method, 101, cash), before_ex, 1e-12);
    const BsmInputs put{OptionType::Put, 100, 110, 1, .05, 0, 0};
    const double after_ex = 120 * std::exp(-.05 * .4) - 100;
    EXPECT_NEAR(binomial_price(put, kAmerican, method, 101, cash), after_ex, 1e-12);
    auto escrowed = call;
    escrowed.spot -= 10 * std::exp(-.05 * .4);
    EXPECT_NEAR(binomial_price(call, kEuropean, method, 101, cash), bsm_price(escrowed), 1e-12);
  }
}

TEST(Binomial, CashDividendOnLatticeDateAllowsExerciseBeforeTheJump) {
  const std::vector<CashDividend> cash{{.4, 10}};
  // Small positive volatility keeps CRR admissible at r=0 and the deep call's
  // payoff linear; positive yield makes exercise before the jump preferable.
  const BsmInputs in{OptionType::Call, 100, 60, 1, 0, .001, .02};
  EXPECT_NEAR(binomial_price(in, kAmerican, kCrr, 100, cash), 40, 1e-10);
  const BsmInputs growing{OptionType::Call, 100, 60, 1, .05, 0, .02};
  EXPECT_NEAR(binomial_price(growing, kAmerican, kCrr, 100, cash),
              100 - 60 * std::exp(-.05 * .4), 1e-9);
}

TEST(Binomial, CashDividendFallbackRetainsEarlyExercise) {
  const std::vector<CashDividend> cash{{.4, 10}};
  const BsmInputs in{OptionType::Call, 100, 60, 1, .05, 0, .00001};
  for (auto method : {kCrr, kLr}) {
    EXPECT_NEAR(binomial_price(in, kAmerican, method, 31, cash),
                100 - 60 * std::exp(-.05 * .4), 1e-10);
  }
}

TEST(Binomial, RejectsInvalidCashSchedulesAndNonpositiveEscrow) {
  const BsmInputs in{OptionType::Call, 100, 95, 1, .04, 0, .2};
  for (const auto& cash : {std::vector<CashDividend>{{.5, -1}},
                          std::vector<CashDividend>{{.5, 110}},
                          std::vector<CashDividend>{{.5, std::numeric_limits<double>::infinity()}},
                          std::vector<CashDividend>{{std::numeric_limits<double>::quiet_NaN(), 1}}}) {
    EXPECT_THROW(static_cast<void>(binomial_price(in, kAmerican, kLr, 31, cash)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(binomial_early_exercise_premium(in, 31, cash)), std::invalid_argument);
  }
}

}  // namespace

#include "openport/md/contract.hpp"

#include <gtest/gtest.h>

namespace {

using openport::md::Date;
using openport::md::parse_osi;
using openport::md::Settlement;
using openport::pricing::ExerciseStyle;
using openport::pricing::OptionType;

TEST(Contract, ParsesCompactIndexSymbol) {
  const auto c = parse_osi("SPXW261005P07405000");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->root, "SPXW");
  EXPECT_EQ(c->underlying, "SPX");
  EXPECT_EQ(c->expiry, (Date{2026, 10, 5}));
  EXPECT_DOUBLE_EQ(c->strike, 7405.0);
  EXPECT_EQ(c->type, OptionType::Put);
  EXPECT_EQ(c->style, ExerciseStyle::European);
  EXPECT_EQ(c->settlement, Settlement::PM);
  EXPECT_EQ(c->osi_symbol(), "SPXW  261005P07405000");
}

TEST(Contract, ParsesPaddedEquitySymbolWithFractionalStrike) {
  const auto c = parse_osi("SPY   260922C00550500");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->root, "SPY");
  EXPECT_EQ(c->underlying, "SPY");
  EXPECT_DOUBLE_EQ(c->strike, 550.5);
  EXPECT_EQ(c->type, OptionType::Call);
  EXPECT_EQ(c->style, ExerciseStyle::American);
  EXPECT_EQ(c->osi_symbol(), "SPY   260922C00550500");
}

TEST(Contract, MonthlySpxIsAmSettledAtTheOpen) {
  const auto monthly = parse_osi("SPX261016C00200000");
  const auto weekly = parse_osi("SPXW261016C00200000");
  ASSERT_TRUE(monthly && weekly);
  EXPECT_EQ(monthly->settlement, Settlement::AM);
  EXPECT_EQ(openport::md::format_timestamp(monthly->expiry_time()), "2026-10-16T13:30:00.000Z");
  EXPECT_EQ(openport::md::format_timestamp(weekly->expiry_time()), "2026-10-16T20:00:00.000Z");
}

TEST(Contract, AdjustedRootsMapToTheirUnderlying) {
  const auto c = parse_osi("SPY1  261218C00500000");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->root, "SPY1");
  EXPECT_EQ(c->underlying, "SPY");
  EXPECT_FALSE(c->standard);
}

TEST(Contract, KnowsIndexUnderlyingsAndTheirRoots) {
  EXPECT_TRUE(openport::md::is_index_underlying("SPX"));
  EXPECT_FALSE(openport::md::is_index_underlying("SPY"));
  EXPECT_EQ(openport::md::option_roots("SPX"), (std::vector<std::string>{"SPX", "SPXW"}));
  EXPECT_EQ(openport::md::option_roots("SPY"), (std::vector<std::string>{"SPY"}));
}

TEST(Contract, OexAndXeoShareAnUnderlyingButKeepTheirExerciseStyles) {
  const auto oex = parse_osi("OEX261016C03000000");
  const auto xeo = parse_osi("XEO261016C03000000");
  ASSERT_TRUE(oex && xeo);
  EXPECT_EQ(oex->style, ExerciseStyle::American);
  EXPECT_EQ(xeo->style, ExerciseStyle::European);
  EXPECT_EQ(oex->settlement, Settlement::PM);
  EXPECT_EQ(xeo->settlement, Settlement::PM);
  EXPECT_EQ(oex->underlying, "OEX");
  EXPECT_EQ(xeo->underlying, "OEX");
  EXPECT_EQ(openport::md::option_roots("OEX"), (std::vector<std::string>{"OEX", "XEO"}));
}

TEST(Contract, RejectsMalformedSymbols) {
  EXPECT_FALSE(parse_osi(""));
  EXPECT_FALSE(parse_osi("SPXW261005X07405000"));  // not C or P
  EXPECT_FALSE(parse_osi("SPXW261305P07405000"));  // month 13
  EXPECT_FALSE(parse_osi("SPXW2610O5P07405000"));  // letter in the date
  EXPECT_FALSE(parse_osi("261005P07405000"));      // no root
  EXPECT_FALSE(parse_osi("TOOLONGROOT261005P07405000"));
}

TEST(Contract, RejectsImpossibleCalendarDates) {
  for (const auto s : {"SPXW260231C00100000", "SPXW250229C00100000", "SPXW260431C00100000"})
    EXPECT_FALSE(parse_osi(s)) << s;
  EXPECT_TRUE(parse_osi("SPXW280229C00100000"));
}

TEST(Contract, AmSettledSeriesStopTradingAtThePreviousRegularClose) {
  using openport::md::format_timestamp;
  const auto monthly = parse_osi("SPX261016C00200000");  // Friday
  const auto vix = parse_osi("VIX261021C00020000");      // Wednesday
  const auto weekly = parse_osi("SPXW261016C00200000");
  const auto after_early_close = parse_osi("SPX261130C00200000");  // the Monday after Nov 27
  ASSERT_TRUE(monthly && vix && weekly && after_early_close);
  EXPECT_EQ(format_timestamp(monthly->last_trade_time()), "2026-10-15T20:15:00.000Z");
  EXPECT_EQ(format_timestamp(vix->last_trade_time()), "2026-10-20T20:15:00.000Z");
  EXPECT_EQ(weekly->last_trade_time(), weekly->expiry_time());
  EXPECT_EQ(format_timestamp(after_early_close->last_trade_time()), "2026-11-27T18:15:00.000Z");
}

TEST(Contract, PmExpiryUsesEarlyClose) {
  const auto c = parse_osi("SPXW261127C00100000");
  ASSERT_TRUE(c);
  EXPECT_EQ(openport::md::format_timestamp(c->expiry_time()), "2026-11-27T18:00:00.000Z");
}

}  // namespace

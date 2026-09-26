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
  EXPECT_EQ(openport::md::option_roots("SPX"), (std::vector<std::string>{"SPX", "SPXW"}));
  for (const auto* symbol : {"SPY", "QQQ", "IWM", "DIA"}) {
    EXPECT_FALSE(openport::md::is_index_underlying(symbol)) << symbol;
    EXPECT_EQ(openport::md::option_roots(symbol), (std::vector<std::string>{symbol}));
  }
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

TEST(Contract, EtfOptionsTradeAndExpireAQuarterHourAfterTheClose) {
  using openport::md::format_timestamp;
  using openport::md::is_late_close_underlying;
  const auto spy = parse_osi("SPY261022C00500000");
  const auto adjusted = parse_osi("SPY1261022C00500000");
  const auto gld = parse_osi("GLD261022C00300000");
  const auto aapl = parse_osi("AAPL261022C00200000");
  const auto index = parse_osi("SPXW261022C05000000");
  const auto early = parse_osi("QQQ261127C00500000");
  ASSERT_TRUE(spy && adjusted && gld && aapl && index && early);
  EXPECT_EQ(format_timestamp(spy->expiry_time()), "2026-10-22T20:15:00.000Z");
  EXPECT_EQ(spy->last_trade_time(), spy->expiry_time());
  EXPECT_EQ(adjusted->expiry_time(), spy->expiry_time());
  EXPECT_EQ(gld->expiry_time(), spy->expiry_time());
  // Single stocks close at 16:00, and expiring index series stop then too.
  EXPECT_EQ(format_timestamp(aapl->expiry_time()), "2026-10-22T20:00:00.000Z");
  EXPECT_EQ(format_timestamp(index->expiry_time()), "2026-10-22T20:00:00.000Z");
  EXPECT_EQ(format_timestamp(early->expiry_time()), "2026-11-27T18:15:00.000Z");
  for (const auto* symbol : {"SPY", "QQQ", "IWM", "DIA"}) {
    SCOPED_TRACE(symbol);
    EXPECT_TRUE(is_late_close_underlying(symbol));
    const auto regular = parse_osi(std::string(symbol) + "261022C00200000");
    const auto short_day = parse_osi(std::string(symbol) + "261127C00200000");
    ASSERT_TRUE(regular && short_day);
    EXPECT_EQ(regular->style, ExerciseStyle::American);
    EXPECT_EQ(regular->settlement, Settlement::PM);
    EXPECT_TRUE(regular->standard);
    EXPECT_EQ(regular->expiry_time(), spy->expiry_time());
    EXPECT_EQ(regular->last_trade_time(), regular->expiry_time());
    EXPECT_EQ(short_day->expiry_time(), early->expiry_time());
    EXPECT_EQ(short_day->last_trade_time(), short_day->expiry_time());
  }
  EXPECT_TRUE(is_late_close_underlying("XLF"));
  EXPECT_TRUE(is_late_close_underlying("NDX"));
  EXPECT_FALSE(is_late_close_underlying("AAPL"));
  EXPECT_FALSE(is_late_close_underlying("SPY1"));
}

}  // namespace

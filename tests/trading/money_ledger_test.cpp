#include <limits>
#include <gtest/gtest.h>

#include "openport/trading/ledger.hpp"
#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
Money m(std::string_view text) { return Money::parse(text); }
TEST(TradingMoney, ExactDecimalRoundtripAndInvalidInput) {
  for (const auto* text : {"0.00", "4.60", "-4.60", "0.000001", "-0.000001", "9223372036854.775807", "-9223372036854.775808"})
    EXPECT_EQ(m(text).str(), text);
  EXPECT_EQ(m("4.60").micros(), 4'600'000);
  EXPECT_EQ(m("+001.200000").str(), "1.20");
  for (const auto* text : {"", ".5", "1.", " 1", "1e2", "NaN", "1.0000001", "--1", "+"})
    EXPECT_THROW(m(text), TradingError) << text;
  EXPECT_THROW(m("9223372036854.775808"), TradingError);
  EXPECT_THROW(m("-9223372036854.775809"), TradingError);
  EXPECT_THROW(m(std::string(1000, '9')), TradingError);
}
TEST(TradingMoney, CheckedArithmeticAndRounding) {
  const auto max = Money::from_micros(std::numeric_limits<std::int64_t>::max());
  const auto min = Money::from_micros(std::numeric_limits<std::int64_t>::min());
  EXPECT_THROW(max + Money::from_micros(1), TradingError);
  EXPECT_THROW(min - Money::from_micros(1), TradingError);
  EXPECT_THROW(-min, TradingError);
  EXPECT_THROW(max * 2, TradingError);
  EXPECT_THROW((void)max.prorate(2, 1), TradingError);
  EXPECT_THROW((void)max.prorate(1, 0), TradingError);
  EXPECT_EQ(max.prorate(2, 2), max);
  EXPECT_EQ(Money::from_micros(3).prorate(1, 2).micros(), 2);
  EXPECT_EQ(Money::from_micros(-3).prorate(1, 2).micros(), -2);
  EXPECT_EQ(Money::from_double(4.60), m("4.60"));
  EXPECT_THROW(Money::from_double(std::numeric_limits<double>::infinity()), TradingError);
  EXPECT_THROW(Money::from_double(1e20), TradingError);
}
TEST(TradingLedger, OpenAddReduceCloseReverseFeesReconcileExactly) {
  const test::ScriptedMarket market;
  Ledger ledger(m("10000"));
  auto reconcile = [&] {
    Money basis;
    for (const auto& [symbol, p] : ledger.positions()) basis = basis + p.basis;
    EXPECT_EQ(ledger.account().cash + basis, m("10000") + ledger.account().realised - ledger.account().fees);
  };
  ledger.fill(market.contract, 2, m("4.00"), m("1.30"));
  EXPECT_EQ(ledger.account().cash, m("9198.70"));
  reconcile();
  ledger.fill(market.contract, 1, m("7.00"), m("0.65"));
  EXPECT_EQ(ledger.positions().at(market.symbol()).basis, m("1500"));
  ledger.fill(market.contract, -1, m("6.00"), m("0.65"));
  EXPECT_EQ(ledger.account().realised, m("100"));
  EXPECT_EQ(ledger.positions().at(market.symbol()).basis, m("1000"));
  reconcile();
  ledger.fill(market.contract, -4, m("8.00"), m("2.60"));
  EXPECT_EQ(ledger.positions().at(market.symbol()).quantity, -2);
  EXPECT_EQ(ledger.positions().at(market.symbol()).basis, m("-1600"));
  EXPECT_EQ(ledger.account().realised, m("700"));
  reconcile();
  ledger.fill(market.contract, 2, m("6.00"), m("1.30"));
  EXPECT_TRUE(ledger.positions().empty());
  EXPECT_EQ(ledger.account().realised, m("1100"));
  EXPECT_EQ(ledger.account().fees, m("6.50"));
  EXPECT_EQ(ledger.account().cash, m("11093.50"));
  reconcile();
}
TEST(TradingLedger, RoundingResidueShortAddsAndStrongOverflowGuarantee) {
  const test::ScriptedMarket market;
  Ledger l(m("10000"));
  l.fill(market.contract, -1, m("0.000001"), {});
  l.fill(market.contract, -2, m("0.000002"), {});
  EXPECT_EQ(l.positions().at(market.symbol()).basis.micros(), -500);
  l.fill(market.contract, 1, {}, {});
  EXPECT_EQ(l.account().realised.micros(), 167);
  EXPECT_EQ(l.positions().at(market.symbol()).basis.micros(), -333);
  l.fill(market.contract, 1, {}, {});
  l.fill(market.contract, 1, {}, {});
  EXPECT_EQ(l.account().realised.micros(), 500);
  EXPECT_TRUE(l.positions().empty());
  const auto cash = l.account().cash;
  EXPECT_THROW(l.fill(market.contract, 2, Money::from_micros(std::numeric_limits<std::int64_t>::max()), {}), TradingError);
  EXPECT_EQ(l.account().cash, cash);
  EXPECT_TRUE(l.positions().empty());
}
TEST(TradingContracts, EligibilityAndTickTable) {
  auto contract = test::ScriptedMarket{}.contract;
  contract.style = pricing::ExerciseStyle::American;
  EXPECT_EQ(eligible(contract).code, Reason::AMERICAN_UNSUPPORTED);
  contract.style = pricing::ExerciseStyle::European;
  contract.standard = false;
  EXPECT_EQ(eligible(contract).code, Reason::NONSTANDARD_UNSUPPORTED);
  contract.standard = true;
  contract.root = "XEO";
  EXPECT_EQ(eligible(contract).code, Reason::ROOT_UNSUPPORTED);
  for (const auto* root : {"SPX", "SPXW", "NDX", "NDXP", "RUT", "RUTW"}) {
    EXPECT_EQ(tick_size(root, m("2.99")), m("0.05"));
    EXPECT_EQ(tick_size(root, m("3")), m("0.10"));
  }
  for (const auto* root : {"XSP", "MRUT"}) {
    EXPECT_EQ(tick_size(root, m("2.99")), m("0.01"));
    EXPECT_EQ(tick_size(root, m("3")), m("0.05"));
  }
  for (const auto* root : {"XND", "DJX", "VIX", "VIXW"}) EXPECT_EQ(tick_size(root, m("3")), m("0.01"));
  contract = test::ScriptedMarket{}.contract;
  contract.multiplier = 50;
  EXPECT_EQ(eligible(contract).code, Reason::INVALID_CONTRACT);
  contract.multiplier = 100;
  contract.strike = 5000.0001;
  EXPECT_EQ(eligible(contract).code, Reason::INVALID_CONTRACT);
}
}  // namespace
}  // namespace openport::trading

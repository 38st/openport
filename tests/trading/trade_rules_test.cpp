#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
SessionConfig config(AccountRules rules = {}) {
  SessionConfig c;
  c.initial_cash = m("10000");
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
struct JournalFile {
  std::filesystem::path directory;
  std::string path;
  JournalFile() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-trade-rules-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    directory = pattern;
    path = (directory / "account.jsonl").string();
  }
  ~JournalFile() { std::filesystem::remove_all(directory); }
};

TEST(TradeRules, HeldCapCountsAveragingInWorkingOrdersAndQuantityChanges) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_contracts_held = 5;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("first", 2), f.time).decision.ok());
  const auto working = s.submit(f.limit("working", 2, "4.00"), f.time);
  ASSERT_TRUE(working.decision.ok());
  ASSERT_TRUE(s.submit(f.limit("last", 1, "4.00"), f.time).decision.ok());
  const auto preview = s.preview(f.market("too-many"), f.time);
  EXPECT_EQ(preview.decision.code, Reason::MAX_CONTRACTS_HELD);
  EXPECT_EQ(preview.decision.actual, 6);
  EXPECT_EQ(preview.decision.limit, 5);
  EXPECT_EQ(preview.decision.scope, "account");
  EXPECT_EQ(s.submit(f.market("too-many"), f.time).decision.code, Reason::MAX_CONTRACTS_HELD);
  OrderChange change;
  change.quantity = 3;
  EXPECT_EQ(s.preview_change(*working.order_id, change, f.time).decision.code, Reason::MAX_CONTRACTS_HELD);
  EXPECT_EQ(s.modify(*working.order_id, change, f.time).decision.actual, 6);
  ASSERT_TRUE(s.cancel(*working.order_id, f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("replacement", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("close", 4, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradeRules, HeldCapCountsComboRatiosShortsAndReversalsButNotExitsOrShares) {
  ScriptedMarket f, g;
  g.contract.strike += 5;
  AccountRules rules;
  rules.max_contracts_held = 3;
  TradingSession s(config(rules), f.time);
  f.seed(s); g.seed(s);
  auto combo = f.limit("combo", 1, "4.40");
  combo.symbol.clear();
  combo.legs = {{f.symbol(), Side::Buy, 2}, {g.symbol(), Side::Sell, 1}};
  ASSERT_TRUE(s.submit(combo, f.time).decision.ok());
  EXPECT_EQ(s.submit(f.market("fourth"), f.time).decision.code, Reason::MAX_CONTRACTS_HELD);
  EXPECT_EQ(s.submit(f.market("reverse", 3, Side::Sell), f.time).decision.actual, 4);
  ASSERT_TRUE(s.submit(g.market("close-short", 1, Side::Buy), f.time).decision.ok());
  auto bracket = f.market("bracket");
  bracket.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3")}, {}}, {}};
  ASSERT_TRUE(s.submit(bracket, f.time).decision.ok());
  EXPECT_EQ(s.submit(f.market("past-exits"), f.time).decision.actual, 4);
  s.on_quotes({}, {}, f.time, {{"SPY", f.time, m("500")}});
  ASSERT_TRUE(s.trade_stock("SPY", 1, f.time).decision.ok());
  ASSERT_TRUE(s.close_positions({}, f.time).decision.ok());
}

TEST(TradeRules, WorkingOrdersShareClosingCapacityOnce) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_contracts_held = 2;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("close", 2, "4.50", Side::Sell), f.time).decision.ok());
  const auto result = s.submit(f.limit("extra-close", 1, "4.50", Side::Sell), f.time);
  EXPECT_EQ(result.decision.code, Reason::MAX_CONTRACTS_HELD);
  EXPECT_EQ(result.decision.actual, 3);
}

TEST(TradeRules, ContractCapSurvivesJournalRecovery) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_contracts_held = 3;
  JournalFile file;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held", 2), f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.limit("reserved", 1, "4.00"), f.time).decision.ok());
  }
  auto s = TradingSession::recover(FileJournal::read(file.path), FileJournal::resume(file.path));
  EXPECT_EQ(s.config().rules, rules);
  EXPECT_EQ(s.submit(f.market("after-restart"), f.time).decision.code, Reason::MAX_CONTRACTS_HELD);
  EXPECT_TRUE(s.submit(f.market("close", 2, Side::Sell), f.time).decision.ok());
}

TEST(TradeRules, DisabledRulesAreAbsentFromJournalBytes) {
  ScriptedMarket f;
  JournalFile file;
  {
    TradingSession s(config(), f.time, FileJournal::create(file.path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("entry", 2), f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.market("close", 2, Side::Sell), f.time).decision.ok());
  }
  const auto recovery = FileJournal::read(file.path);
  for (const auto& record : recovery.records) EXPECT_EQ(record.payload.find("max_contracts_held"), std::string::npos);
  auto s = TradingSession::recover(recovery);
  EXPECT_EQ(s.config().rules.max_contracts_held, 0);
}

TEST(TradeRules, ContractCapValidatesItsRange) {
  AccountRules rules;
  for (const Quantity cap : {-1, 100001}) {
    rules.max_contracts_held = cap;
    EXPECT_THROW(validate_rules(rules), TradingError);
  }
  rules.max_contracts_held = 100000;
  EXPECT_NO_THROW(validate_rules(rules));
}
}  // namespace
}  // namespace openport::trading

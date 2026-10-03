#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "support/scripted_market.hpp"
#include "trading/state.hpp"

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

OrderRequest stopped(OrderRequest request, std::string_view level = "3.00", TriggerSource source = TriggerSource::Option) {
  const auto direction = multi_leg(request) || request.side == Side::Sell ? TriggerDirection::AtOrAbove : TriggerDirection::AtOrBelow;
  request.bracket = Bracket{ExitSpec{Trigger{source, direction, m(level)}, {}}, {}};
  return request;
}

TEST(TradeRules, RequiredStopsProtectEntriesAndCannotBeCancelledWhileHeld) {
  ScriptedMarket f;
  AccountRules rules;
  rules.require_stop_loss = true;
  TradingSession s(config(rules), f.time);
  f.seed(s, "4.00", "4.20", 1);
  EXPECT_EQ(s.preview(f.market("bare"), f.time).decision.code, Reason::STOP_REQUIRED);
  EXPECT_EQ(s.submit(f.market("bare"), f.time).decision.code, Reason::STOP_REQUIRED);
  auto target = f.market("target-only");
  target.bracket = Bracket{{}, ExitSpec{{}, m("5.00")}};
  EXPECT_EQ(s.submit(target, f.time).decision.code, Reason::STOP_REQUIRED);
  target.client_order_id = "limit-only";
  target.bracket = Bracket{ExitSpec{{}, m("3.00")}, {}};
  EXPECT_EQ(s.submit(target, f.time).decision.code, Reason::STOP_REQUIRED);
  auto wrong = stopped(f.market("wrong-way"));
  wrong.bracket->stop_loss->trigger->direction = TriggerDirection::AtOrAbove;
  EXPECT_EQ(s.submit(wrong, f.time).decision.code, Reason::STOP_REQUIRED);
  const auto entry = s.submit(stopped(f.limit("protected", 2)), f.time);
  ASSERT_TRUE(entry.decision.ok());
  const auto stop = s.snapshot()->recent_orders.at(*entry.order_id - 1).stop_loss;
  ASSERT_NE(stop, 0U);
  EXPECT_EQ(s.cancel(stop, f.time).decision.code, Reason::STOP_REQUIRED);
  EXPECT_EQ(s.cancel_all({}, f.time).decision.code, Reason::STOP_REQUIRED);
  EXPECT_EQ(s.cancel_orders({*entry.order_id, stop}, f.time).decision.code, Reason::STOP_REQUIRED);
  // Cancelling the unfilled entry is safe: its filled part keeps its stop.
  EXPECT_TRUE(s.cancel(*entry.order_id, f.time).decision.ok());
  EXPECT_TRUE(s.submit(f.market("manual-close", 1, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_TRUE(s.cancel_all({}, f.time).decision.ok());
  s.on_quotes({}, {}, f.time, {{"SPY", f.time, m("500")}});
  EXPECT_EQ(s.trade_stock("SPY", 1, f.time).decision.code, Reason::STOP_REQUIRED);
}

TEST(TradeRules, StopFillsRearmPartialExitsAndFlattenRemainsAvailable) {
  ScriptedMarket f;
  AccountRules rules;
  rules.require_stop_loss = true;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  const auto entry = s.submit(stopped(f.market("entry", 2)), f.time);
  ASSERT_TRUE(entry.decision.ok());
  const auto stop = s.snapshot()->recent_orders.at(*entry.order_id - 1).stop_loss;
  OrderChange shrink; shrink.quantity = 1;
  EXPECT_EQ(s.modify(stop, shrink, f.time).decision.code, Reason::STOP_REQUIRED);
  f.next();
  s.on_quotes({f.quote("2.90", "3.00", 1)}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->positions.at(0).position.quantity, 1);
  EXPECT_EQ(s.snapshot()->recent_orders.at(stop - 1).status, OrderStatus::Armed);
  EXPECT_EQ(s.cancel(stop, f.time).decision.code, Reason::STOP_REQUIRED);
  f.next();
  s.on_quotes({f.quote("3.10", "3.20")}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.close_positions({}, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradeRules, StopRiskUsesExactEntryTermsAndExcludesFees) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_trade_risk = m("120");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  auto request = stopped(f.market("at-cap"));
  const auto preview = s.preview(request, f.time);
  EXPECT_TRUE(preview.decision.ok());
  EXPECT_EQ(preview.trade_risk, m("120"));
  EXPECT_EQ(preview.trade_risk_limit, m("120"));
  EXPECT_EQ(preview.trade_risk_basis, "stop_loss");
  EXPECT_EQ(preview.max_units, 1);
  request.quantity = 2;
  const auto refused = s.submit(request, f.time);
  EXPECT_EQ(refused.decision.code, Reason::MAX_TRADE_RISK);
  EXPECT_EQ(refused.decision.actual, 240);
  EXPECT_EQ(refused.decision.limit, 120);
  EXPECT_EQ(refused.decision.scope, "trade");
  // A resting limit uses its limit, not today's ask; a change rechecks the total quantity.
  const auto working = s.submit(stopped(f.limit("resting", 1, "4.00")), f.time);
  ASSERT_TRUE(working.decision.ok());
  OrderChange change; change.quantity = 2;
  EXPECT_EQ(s.preview_change(*working.order_id, change, f.time).trade_risk, m("200"));
  EXPECT_EQ(s.modify(*working.order_id, change, f.time).decision.code, Reason::MAX_TRADE_RISK);
  EXPECT_EQ(s.submit(stopped(f.market("short", 1, Side::Sell), "5.20"), f.time).decision.code, Reason::NONE);
}

TEST(TradeRules, MissingAndUnderlyingStopsUseBoundedExpiryLossNeverTheScenarioGrid) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_trade_risk = m("420");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  auto preview = s.preview(f.market("long"), f.time);
  EXPECT_TRUE(preview.decision.ok());
  EXPECT_EQ(preview.trade_risk, m("420"));
  EXPECT_EQ(preview.trade_risk_basis, "expiry_payoff");
  preview = s.preview(f.market("short", 1, Side::Sell), f.time);
  EXPECT_EQ(preview.decision.code, Reason::MAX_TRADE_RISK);
  EXPECT_FALSE(preview.trade_risk);
  EXPECT_EQ(preview.trade_risk_basis, "unbounded_or_unknown");
  EXPECT_EQ(s.submit(f.market("short", 1, Side::Sell), f.time).decision.code, Reason::MAX_TRADE_RISK);
  EXPECT_EQ(s.preview(stopped(f.market("spot-stop"), "4900", TriggerSource::Underlying), f.time).trade_risk, m("420"));
}

TEST(TradeRules, ComboRiskUsesSignedNetStopAndExpiryPayoff) {
  ScriptedMarket f, g;
  g.contract.strike += 5;
  AccountRules rules;
  rules.require_stop_loss = true;
  rules.max_trade_risk = m("110");
  TradingSession s(config(rules), f.time);
  f.seed(s, "4.00", "4.20"); g.seed(s, "2.00", "2.20");
  auto combo = f.limit("credit", 1, "-1.80");
  combo.symbol.clear(); combo.legs = {{f.symbol(), Side::Sell, 1}, {g.symbol(), Side::Buy, 1}};
  EXPECT_EQ(s.submit(combo, f.time).decision.code, Reason::STOP_REQUIRED);
  combo = stopped(combo, "3.00", TriggerSource::Combo);
  combo.client_order_id = "credit-stop";
  EXPECT_EQ(s.preview(combo, f.time).trade_risk, m("120"));
  EXPECT_EQ(s.submit(combo, f.time).decision.code, Reason::MAX_TRADE_RISK);
  combo.client_order_id = "credit-safe";
  combo.bracket->stop_loss->trigger->level = m("2.90");
  ASSERT_TRUE(s.submit(combo, f.time).decision.ok());
  EXPECT_TRUE(s.close_positions({}, f.time).decision.ok());
  auto debit = combo;
  debit.client_order_id = "debit"; debit.limit_price = m("2.20");
  for (auto& leg : debit.legs) leg.side = leg.side == Side::Buy ? Side::Sell : Side::Buy;
  debit.bracket->stop_loss->trigger->level = m("-1.00");
  EXPECT_EQ(s.preview(debit, f.time).trade_risk, m("120"));
  rules.require_stop_loss = false; rules.max_trade_risk = m("320");
  TradingSession t(config(rules), f.time);
  f.seed(t); g.seed(t, "2.00", "2.20");
  combo.bracket.reset(); combo.client_order_id = "bounded";
  EXPECT_EQ(t.preview(combo, f.time).trade_risk, m("320"));
  EXPECT_TRUE(t.submit(combo, f.time).decision.ok());
}

TEST(TradeRules, PercentageUsesCurrentPlanFloorRoomAndTheTighterLimit) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_drawdown = m("1000"); rules.drawdown_mode = DrawdownMode::Static;
  rules.max_trade_risk_percent = 10; rules.max_trade_risk = m("120");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  auto request = stopped(f.market("entry"));
  EXPECT_EQ(s.preview(request, f.time).trade_risk_limit, m("100"));
  EXPECT_EQ(s.submit(request, f.time).decision.code, Reason::MAX_TRADE_RISK);
  request.client_order_id = "fits"; request.bracket->stop_loss->trigger->level = m("3.20");
  EXPECT_TRUE(s.submit(request, f.time).decision.ok());
  EXPECT_EQ(s.preview(stopped(f.market("another")), f.time).trade_risk_limit, m("98.935"));
  rules.max_drawdown = {}; rules.max_trade_risk = {};
  TradingSession t(config(rules), f.time);
  f.seed(t);
  EXPECT_FALSE(t.preview(request, f.time).trade_risk_limit);
  EXPECT_TRUE(t.submit(f.market("no-floor", 1, Side::Sell), f.time).decision.ok());
}

TEST(TradeRules, StopAndRiskRulesSurviveJournalRecovery) {
  ScriptedMarket f;
  AccountRules rules;
  rules.require_stop_loss = true; rules.max_trade_risk = m("250"); rules.max_trade_risk_percent = 25;
  rules.max_contracts_held = 5;
  JournalFile file;
  OrderId stop = 0;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    f.seed(s);
    const auto result = s.submit(stopped(f.market("entry")), f.time);
    ASSERT_TRUE(result.decision.ok());
    stop = s.snapshot()->recent_orders.at(*result.order_id - 1).stop_loss;
  }
  auto s = TradingSession::recover(FileJournal::read(file.path), FileJournal::resume(file.path));
  EXPECT_EQ(s.config().rules, rules);
  EXPECT_EQ(s.cancel(stop, f.time).decision.code, Reason::STOP_REQUIRED);
  EXPECT_EQ(s.submit(f.market("bare"), f.time).decision.code, Reason::STOP_REQUIRED);
  EXPECT_EQ(s.submit(stopped(f.market("over-risk", 3)), f.time).decision.code, Reason::MAX_TRADE_RISK);
  EXPECT_TRUE(s.close_positions({}, f.time).decision.ok());
}

TEST(TradeRules, RiskRulesValidateRanges) {
  AccountRules rules;
  rules.max_trade_risk = m("-0.000001");
  EXPECT_THROW(validate_rules(rules), TradingError);
  rules.max_trade_risk = {};
  for (const auto percent : {-1, 101}) {
    rules.max_trade_risk_percent = percent;
    EXPECT_THROW(validate_rules(rules), TradingError);
  }
}

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
  EXPECT_EQ(preview.max_units, 0);
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
  for (const auto& record : recovery.records)
    for (const auto* field : {"max_contracts_held", "require_stop_loss", "max_trade_risk", "max_trade_risk_percent"})
      EXPECT_EQ(record.payload.find(field), std::string::npos);
  auto s = TradingSession::recover(recovery);
  EXPECT_EQ(s.config().rules.max_contracts_held, 0);
  // An older journal's literal rules re-encode byte for byte, without new defaults.
  const auto legacy = FileJournal::read((std::filesystem::path(OPENPORT_TEST_DATA_DIR) / "kill-before-reduce-only.jsonl").string());
  for (const auto& record : legacy.records) {
    const auto j = nlohmann::json::parse(record.payload);
    if (!j.contains("state")) continue;
    const auto& old_rules = j.at("state").at("config").at("rules");
    const nlohmann::json encoded = old_rules.get<AccountRules>();
    EXPECT_EQ(encoded.dump(), old_rules.dump());
  }
  JournalFile repeated;
  auto defaults = config();
  defaults.rules.max_contracts_held = 0;
  defaults.rules.require_stop_loss = false;
  defaults.rules.max_trade_risk = {};
  defaults.rules.max_trade_risk_percent = 0;
  {
    TradingSession t(defaults, f.time, FileJournal::create(repeated.path));
    f.seed(t);
    ASSERT_TRUE(t.submit(f.market("entry", 2), f.time).decision.ok());
    ASSERT_TRUE(t.submit(f.market("close", 2, Side::Sell), f.time).decision.ok());
  }
  const auto again = FileJournal::read(repeated.path);
  ASSERT_EQ(again.records.size(), recovery.records.size());
  for (std::size_t i = 0; i < recovery.records.size(); ++i) {
    EXPECT_EQ(again.records[i].payload, recovery.records[i].payload);
    EXPECT_EQ(again.records[i].hash, recovery.records[i].hash);
  }
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

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "openport/trading/events.hpp"
#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig config(AccountRules rules) {
  SessionConfig c; c.initial_cash = m("10000"); c.limits.aggregate = {1e9, 1e9}; c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules); return c;
}
AccountRules news(const ScriptedMarket& f, std::string action = "block", std::string symbol = "") {
  AccountRules r; r.news_before_minutes = 1; r.news_after_minutes = 1; r.news_action = std::move(action);
  r.events = {{"news", md::format_timestamp(f.time + 2 * md::kNanosPerMinute), std::move(symbol), "", "CPI"}}; return r;
}
void tick(TradingSession& s, ScriptedMarket& f, Timestamp time) {
  f.time = time; ++f.observation; s.on_quotes({f.quote()}, {f.valuation()}, time);
}
TEST(EventRules, NewsRefusesPreviewCancelsOpeningsAndAllowsReductions) {
  ScriptedMarket f; auto r = news(f); TradingSession s(config(r), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held", 2), f.time).decision.ok());
  const auto open = s.submit(f.limit("working", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time);
  const auto close = s.submit(f.limit("exit", 1, "8", Side::Sell, TimeInForce::Gtc), f.time);
  tick(s, f, f.time + md::kNanosPerMinute);
  EXPECT_EQ(s.snapshot()->recent_orders.at(*open.order_id - 1).reason.code, Reason::NEWS_BLACKOUT);
  EXPECT_TRUE(s.snapshot()->recent_orders.at(*close.order_id - 1).open());
  const auto preview = s.preview(f.market("blocked"), f.time);
  EXPECT_EQ(preview.decision.code, Reason::NEWS_BLACKOUT); EXPECT_EQ(preview.decision.scope, "account");
  EXPECT_EQ(s.submit(f.market("blocked"), f.time).decision.code, Reason::NEWS_BLACKOUT);
  EXPECT_TRUE(s.submit(f.market("reduce", 1, Side::Sell), f.time).decision.ok());
  tick(s, f, f.time + 2 * md::kNanosPerMinute);
  EXPECT_TRUE(s.submit(f.market("end-exclusive"), f.time).decision.ok());
}
TEST(EventRules, SymbolScopesNewsAndShares) {
  ScriptedMarket f; auto r = news(f, "block", "SPY"); TradingSession s(config(r), f.time); f.seed(s);
  tick(s, f, f.time + md::kNanosPerMinute);
  EXPECT_TRUE(s.submit(f.market("spx"), f.time).decision.ok());
  EXPECT_EQ(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("500")}).decision.code, Reason::NEWS_BLACKOUT);
  EXPECT_TRUE(s.trade_stock("QQQ", 1, f.time, StockPrice{"QQQ", f.time, m("450")}).decision.ok());
}
TEST(EventRules, GapOverNewsDoesNotFlattenOrCancelWorkingOpenings) {
  ScriptedMarket f; auto r = news(f, "flatten"); TradingSession s(config(r), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  const auto open = s.submit(f.limit("working", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time);
  tick(s, f, f.time + 4 * md::kNanosPerMinute);
  EXPECT_EQ(s.snapshot()->positions.size(), 1U); EXPECT_TRUE(s.snapshot()->recent_orders.at(*open.order_id - 1).open());
  EXPECT_TRUE(s.snapshot()->evaluation.event_actions.empty());
}
struct JournalFile {
  std::filesystem::path dir;
  std::string path;
  JournalFile() {
    auto pattern = (std::filesystem::temp_directory_path() / "openport-events-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
    dir = pattern; path = (dir / "journal.jsonl").string();
  }
  ~JournalFile() { std::filesystem::remove_all(dir); }
};
TEST(EventRules, FlattenOnceWithRecoveryAndIdenticalReplay) {
  ScriptedMarket f; auto r = news(f, "flatten"); JournalFile file;
  std::string before;
  {
    TradingSession s(config(r), f.time, FileJournal::create(file.path)); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
    tick(s, f, f.time + md::kNanosPerMinute);
    EXPECT_TRUE(s.snapshot()->positions.empty());
    EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:news:"));
    before = s.snapshot_json();
  }
  auto s = TradingSession::recover(FileJournal::read(file.path), FileJournal::resume(file.path));
  EXPECT_EQ(s.snapshot_json(), before);
  const auto orders = s.snapshot()->recent_orders.size();
  tick(s, f, f.time + md::kNanosPerSecond);
  EXPECT_EQ(s.snapshot()->recent_orders.size(), orders);
  EXPECT_EQ(s.snapshot()->evaluation.event_actions.size(), 1U);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
}
TEST(EventRules, EachCorporateKindAndEarningsSessionHasCorrectCutoff) {
  for (const auto& kind : {"earnings", "ex_dividend", "split"}) {
    for (const auto& session : {"before_open", "after_close"}) {
      if (std::string(kind) != "earnings" && std::string(session) == "after_close") continue;
      AccountRules r; r.hold_restrictions = {kind};
      r.events = {{kind, "2026-09-23", "SPY", std::string(kind) == "earnings" ? session : "", "event"}};
      normalize_event_rules(r, true);
      const auto day = std::string(session) == "after_close" ? md::Date{2026, 9, 23} : md::Date{2026, 9, 22};
      const auto at = md::new_york_to_utc(day, 15, 45);
      EXPECT_TRUE(event_entry_check(r, "SPY", at - 1).ok());
      const auto d = event_entry_check(r, "SPY", at);
      EXPECT_EQ(d.code, Reason::HOLD_RESTRICTED); EXPECT_EQ(d.scope, std::string(kind) + ":SPY");
      EXPECT_TRUE(event_entry_check(r, "SPX", at).ok());
      EXPECT_TRUE(event_entry_check(r, "SPY", md::new_york_to_utc(day, 17, 0)).ok());
    }
  }
}
TEST(EventRules, WeekendHolidayAndFrozenBusinessCalendar) {
  AccountRules r; r.hold_restrictions = {"weekend"}; normalize_event_rules(r, true);
  EXPECT_EQ(event_entry_check(r, "SPY", md::new_york_to_utc({2026, 4, 2}, 15, 45)).code, Reason::HOLD_RESTRICTED); // Good Friday
  EXPECT_TRUE(event_entry_check(r, "SPY", md::new_york_to_utc({2026, 4, 1}, 15, 45)).ok());
  EXPECT_EQ(event_entry_check(r, "SPY", md::new_york_to_utc({2026, 9, 25}, 15, 45)).code, Reason::HOLD_RESTRICTED);
  md::ScheduledDaysScope changed({{{2026, 9, 25}, "New closure", true, 13, 0}});
  EXPECT_TRUE(event_entry_check(r, "SPY", md::new_york_to_utc({2026, 9, 24}, 15, 45)).ok());
  EXPECT_EQ(event_entry_check(r, "SPY", md::new_york_to_utc({2026, 9, 25}, 15, 45)).code, Reason::HOLD_RESTRICTED);
}
TEST(EventRules, HoldingCancelsAndClosesOptionsAndSharesOnce) {
  for (const auto& kind : {"earnings", "ex_dividend", "split", "weekend"}) {
    ScriptedMarket f; f.contract = *md::parse_osi("SPY261022C00500000"); f.time = md::new_york_to_utc({2026, 9, 25}, 15, 44);
    AccountRules r; r.hold_restrictions = {kind};
    if (std::string(kind) != "weekend") r.events = {{kind, "2026-09-28", "SPY", "", ""}};
    TradingSession s(config(r), f.time); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
    ASSERT_TRUE(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
    const auto open = s.submit(f.limit("working", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time);
    f.time += md::kNanosPerMinute; ++f.observation;
    s.on_quotes({f.quote()}, {f.valuation()}, f.time, {{"SPY", f.time, m("500")}});
    EXPECT_TRUE(s.snapshot()->positions.empty()); EXPECT_TRUE(s.snapshot()->stocks.empty());
    EXPECT_EQ(s.snapshot()->recent_orders.at(*open.order_id - 1).reason.code, Reason::HOLD_RESTRICTED);
    EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:hold:"));
    const auto count = s.snapshot()->recent_orders.size(); tick(s, f, f.time + md::kNanosPerSecond);
    EXPECT_EQ(s.snapshot()->recent_orders.size(), count);
    EXPECT_EQ(s.preview(f.market("no-entry"), f.time).decision.code, Reason::HOLD_RESTRICTED);
  }
}
TEST(EventRules, GapAcrossHoldingBoundaryFailsAtRolloverEvenAfterLateClose) {
  ScriptedMarket f; f.time = md::new_york_to_utc({2026, 9, 25}, 15, 44);
  AccountRules r; r.hold_restrictions = {"weekend"}; TradingSession s(config(r), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  tick(s, f, md::new_york_to_utc({2026, 9, 28}, 10, 0));
  EXPECT_TRUE(s.snapshot()->positions.empty()); // missed cutoff executes once with Monday liquidity
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::HOLD_RESTRICTED);
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
}
TEST(EventRules, FailedCutoffLiquidityFailsAtRollover) {
  ScriptedMarket f; f.time = md::new_york_to_utc({2026, 9, 25}, 15, 44);
  AccountRules r; r.hold_restrictions = {"weekend"}; TradingSession s(config(r), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 9, 25}, 15, 45));
  ASSERT_FALSE(s.snapshot()->positions.empty());
  ASSERT_TRUE(s.roll_day(md::new_york_to_utc({2026, 9, 25}, 17, 0)).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::HOLD_RESTRICTED);
}
TEST(EventRules, SettlementAwaitingPositionIsSkipped) {
  ScriptedMarket f; f.contract = *md::parse_osi("SPXW260922C05000000");
  AccountRules r; r.news_after_minutes = 10; r.news_action = "flatten";
  r.events = {{"news", "2026-09-22T20:01:00Z", "", "", ""}};
  TradingSession s(config(r), f.time); f.seed(s); ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  tick(s, f, md::new_york_to_utc({2026, 9, 22}, 16, 1));
  EXPECT_EQ(s.snapshot()->positions.size(), 1U); EXPECT_EQ(s.snapshot()->recent_orders.size(), 1U);
  EXPECT_EQ(s.snapshot()->evaluation.event_actions.size(), 1U);
}
TEST(EventRules, HoldingSkipsSettlementAwaitingPositionAndRecordsReplayIdentically) {
  ScriptedMarket f; f.contract = *md::parse_osi("SPXW260922C05000000");
  AccountRules r; r.hold_restrictions = {"split"}; r.hold_cutoff = 16 * 60 + 5;
  r.events = {{"split", "2026-09-23", "SPX"}};
  JournalFile first, second;
  const auto run = [&](const std::string& path) {
    TradingSession s(config(r), f.time, FileJournal::create(path)); f.seed(s);
    EXPECT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
    const auto cutoff = md::new_york_to_utc({2026, 9, 22}, 16, 5);
    s.on_quotes({}, {}, cutoff);
    EXPECT_EQ(s.snapshot()->positions.size(), 1U); EXPECT_EQ(s.snapshot()->recent_orders.size(), 1U);
    EXPECT_EQ(s.snapshot()->evaluation.event_actions.size(), 1U);
  };
  run(first.path); run(second.path);
  EXPECT_EQ(FileJournal::read(first.path).head, FileJournal::read(second.path).head);
  auto recovered = TradingSession::recover(FileJournal::read(first.path));
  EXPECT_EQ(recovered.snapshot()->evaluation.event_actions.size(), 1U);
  recovered.on_quotes({}, {}, md::new_york_to_utc({2026, 9, 22}, 16, 6));
  EXPECT_EQ(recovered.snapshot()->recent_orders.size(), 1U);
}
TEST(EventRules, OverlappingNewsAndDuplicateHoldingScopesActDeterministically) {
  ScriptedMarket f; auto r = news(f);
  r.events.push_back({"news", md::format_timestamp(f.time + 3 * md::kNanosPerMinute), "SPY", "", "FOMC"});
  normalize_event_rules(r);
  EXPECT_EQ(event_entry_check(r, "QQQ", f.time + md::kNanosPerMinute).scope, "account");
  EXPECT_TRUE(event_entry_check(r, "QQQ", f.time + 3 * md::kNanosPerMinute).ok());
  EXPECT_EQ(event_entry_check(r, "SPY", f.time + 3 * md::kNanosPerMinute).code, Reason::NEWS_BLACKOUT);
  r.hold_restrictions = {"split"}; r.events = {{"split", "2026-09-23", "SPY", "", "First"}, {"split", "2026-09-23", "SPY", "", "Second"}};
  normalize_event_rules(r, true);
  EXPECT_EQ(event_windows(r, md::new_york_to_utc({2026, 9, 22}, 15, 45), md::new_york_to_utc({2026, 9, 22}, 15, 45)).size(), 1U);
}
TEST(EventRules, NormalizeAndValidateCalendar) {
  AccountRules r; r.events = {{"earnings", "2026-09-23", "SPY"}, {"news", "2026-09-22T14:00:00Z"}, {"earnings", "2026-09-23", "SPY", "before_open"}};
  normalize_event_rules(r); ASSERT_EQ(r.events.size(), 2U); EXPECT_EQ(r.events[0].kind, "news"); EXPECT_EQ(r.events[1].session, "before_open");
  r.news_before_minutes = 241; EXPECT_THROW(validate_event_rules(r), TradingError); r.news_before_minutes = 0;
  r.events[1].symbol = "spy"; EXPECT_THROW(validate_event_rules(r), TradingError);
}
}  // namespace
}  // namespace openport::trading

#include <filesystem>
#include <fstream>
#include <sstream>
#include <gtest/gtest.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
class TemporaryJournal {
 public:
  TemporaryJournal() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-trading-XXXXXX").string();
    auto* directory = ::mkdtemp(pattern.data());
    if (!directory) throw std::runtime_error("mkdtemp failed");
    directory_ = directory;
    path = (directory_ / "session.jsonl").string();
  }
  ~TemporaryJournal() { std::filesystem::remove_all(directory_); }
  std::string read() const {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text; text << file.rdbuf(); return text.str();
  }
  void write(std::string_view text) const { std::ofstream file(path, std::ios::binary); file << text; }
  std::string path;
 private:
  std::filesystem::path directory_;
};
class FailingJournal final : public Journal {
 public:
  bool fail = false;
  std::uint64_t count = 0;
  void append(Timestamp, std::string_view, std::string_view) override {
    if (fail) throw TradingError(Reason::JOURNAL_IO, "injected disk failure");
    ++count;
  }
  std::uint64_t sequence() const override { return count; }
  std::string head() const override { return std::string(64, '0'); }
};
TEST(TradingJournal, ScriptedEndToEndOutcomeRecoveryProducesIdenticalSnapshot) {
  TemporaryJournal file;
  test::ScriptedMarket f;
  std::string expected;
  std::string head;
  {
    auto journal = FileJournal::create(file.path);
    TradingSession s({}, f.time, journal);
    f.seed(s, "4", "4.20", 1);
    s.submit(f.limit("partial", 3), f.time);
    s.submit(f.limit("rest", 1, "4.10"), f.time);
    s.submit(f.limit("rejected", 1, "4.21"), f.time);
    f.next();
    s.on_quotes({f.quote("4", "4.20", 1)}, {f.valuation()}, f.time);
    auto limits = Limits{}; limits.max_order_contracts = 20;
    s.set_limits(limits, f.time);
    s.cancel(2, f.time);
    s.trip_kill("operator review", f.time);
    s.reset_kill("review complete", f.time);
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto recovery = FileJournal::read(file.path, head);
  EXPECT_FALSE(recovery.truncated_final_line);
  ASSERT_FALSE(recovery.records.empty());
  const auto text = file.read();
  for (const auto* type : {"session_start", "order_accepted", "order_rejected", "fill", "cancel", "limit_change", "kill_trip", "kill_reset"})
    EXPECT_NE(text.find(std::string("\"type\":\"") + type + "\""), std::string::npos) << type;
  {
    auto resumed = FileJournal::resume(file.path);
    auto s = TradingSession::recover(recovery, resumed);
    EXPECT_EQ(s.snapshot_json(), expected);
    s.submit(f.market("empty-budget"), f.time);
    EXPECT_EQ(s.snapshot()->recent_orders.back().reason.code, Reason::IOC_REMAINDER);
    EXPECT_EQ(s.snapshot()->recent_fills.size(), 2);
    f.next();
    s.on_quotes({f.quote()}, {f.valuation()}, f.time);
    s.submit(f.market("new"), f.time);
    f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
    ++f.observation;
    s.on_quotes({f.quote()}, {f.valuation()}, f.time);
    s.roll_day(f.time);
    f.time = f.contract.expiry_time();
    s.on_quotes({}, {}, f.time);
    s.settle(f.symbol(), Money::parse("5010"), f.time);
    expected = s.snapshot_json();
  }
  auto replayed = TradingSession::recover(FileJournal::read(file.path));
  EXPECT_EQ(replayed.snapshot_json(), expected);
  EXPECT_TRUE(replayed.snapshot()->positions.empty());
}
TEST(TradingJournal, IdenticalInputsProduceIdenticalBytesAndRecoveryPreservesLiquidityAndKill) {
  TemporaryJournal a;
  TemporaryJournal b;
  test::ScriptedMarket f;
  auto script = [&](const std::string& path) {
    auto journal = FileJournal::create(path);
    TradingSession s({}, f.time, journal);
    f.seed(s, "4", "4.20", 1);
    s.submit(f.limit("partial", 2), f.time);
    return s.snapshot_json();
  };
  EXPECT_EQ(script(a.path), script(b.path));
  EXPECT_EQ(a.read(), b.read());
  auto resumed = FileJournal::resume(a.path);
  auto s = TradingSession::recover(FileJournal::read(a.path), resumed);
  s.on_quotes({f.quote("4", "4.20", 100)}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders[0].filled_quantity, 1);
  EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::PartiallyFilled);
  s.trip_kill("latched", f.time);
  const auto replayed = TradingSession::recover(FileJournal::read(a.path));
  EXPECT_TRUE(replayed.snapshot()->risk.kill_latched);
  EXPECT_EQ(replayed.snapshot_json(), s.snapshot_json());
}
TEST(TradingJournal, HashTamperingSequenceReorderingAndTrustedHeadDetectDamage) {
  TemporaryJournal file;
  test::ScriptedMarket f;
  std::string head;
  {
    auto journal = FileJournal::create(file.path);
    TradingSession s({}, f.time, journal);
    f.seed(s);
    head = journal->head();
  }
  const auto good = file.read();
  auto tampered = good;
  const auto cash = tampered.find("100000000000");
  ASSERT_NE(cash, std::string::npos);
  tampered[cash] = '2';
  EXPECT_THROW((void)verify_journal(tampered), TradingError);
  const auto newline = good.find('\n');
  EXPECT_THROW((void)verify_journal(good.substr(newline + 1)), TradingError);
  EXPECT_THROW((void)verify_journal(good.substr(0, newline + 1), head), TradingError);
  EXPECT_THROW((void)verify_journal("{}\n"), TradingError);
  EXPECT_THROW((void)verify_journal(good + "broken\n"), TradingError);
  auto recovery = verify_journal(good);
  recovery.records.back().payload = "{}";
  EXPECT_THROW(TradingSession::recover(recovery), TradingError);
}
TEST(TradingJournal, TruncatedFinalLineIsReportedIgnoredAndNeverSilentlyOverwritten) {
  TemporaryJournal file;
  test::ScriptedMarket f;
  std::string snapshot;
  {
    auto journal = FileJournal::create(file.path);
    TradingSession s({}, f.time, journal);
    f.seed(s);
    snapshot = s.snapshot_json();
  }
  const auto good = file.read();
  file.write(good + "{\"seq\":4,");
  const auto recovered = FileJournal::read(file.path);
  EXPECT_TRUE(recovered.truncated_final_line);
  EXPECT_EQ(TradingSession::recover(recovered).snapshot_json(), snapshot);
  EXPECT_THROW(FileJournal::resume(file.path), TradingError);
  EXPECT_EQ(file.read(), good + "{\"seq\":4,");
  // A fully serialized but un-terminated record is also an uncommitted suffix.
  const auto no_newline = verify_journal(good.substr(0, good.size() - 1));
  EXPECT_TRUE(no_newline.truncated_final_line);
  EXPECT_EQ(no_newline.records.size() + 1, recovered.records.size());
}
TEST(TradingJournal, WriteFailureStopsTradingWithoutPublishingUnjournalledEffects) {
  test::ScriptedMarket f;
  auto journal = std::make_shared<FailingJournal>();
  TradingSession s({}, f.time, journal);
  f.seed(s);
  const auto before = s.snapshot();
  journal->fail = true;
  EXPECT_THROW(s.submit(f.market("failure"), f.time), TradingError);
  EXPECT_EQ(s.snapshot()->account_version, before->account_version);
  EXPECT_EQ(s.snapshot()->account.cash, before->account.cash);
  EXPECT_TRUE(s.snapshot()->recent_orders.empty());
  EXPECT_TRUE(s.snapshot()->journal_failed);
  journal->fail = false;
  EXPECT_THROW(s.submit(f.market("still-stopped"), f.time), TradingError);
  EXPECT_THROW(s.trip_kill("cannot write", f.time), TradingError);
  EXPECT_FALSE(before->journal_failed);
}
TEST(TradingJournal, ExclusiveWriterAndInvalidPathsFailLoudly) {
  TemporaryJournal file;
  auto journal = FileJournal::create(file.path);
  EXPECT_THROW(FileJournal::create(file.path), TradingError);
  EXPECT_THROW(FileJournal::resume(file.path), TradingError);
  EXPECT_THROW(FileJournal::create(file.path + "/missing"), TradingError);
  EXPECT_THROW(FileJournal::read(file.path + ".missing"), TradingError);
}
}  // namespace
}  // namespace openport::trading

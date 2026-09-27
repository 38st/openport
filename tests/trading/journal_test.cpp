#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <gtest/gtest.h>
#include "trading/state.hpp"
#include <nlohmann/json.hpp>

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
TEST(TradingJournal, BatchedWritesAreImmediatelyReadableAndFlushOnlyWhenPending) {
  TemporaryJournal file;
  int syncs = 0;
  FileJournal::Options options;
  options.sync_policy = FileJournal::SyncPolicy::Batched;
  options.sync_interval = std::chrono::hours(1);
  options.hooks.sync = [&](int) { ++syncs; return true; };
  options.hooks.clock = [] { return std::chrono::steady_clock::time_point{}; };
  auto journal = FileJournal::create(file.path, options);
  journal->flush();
  EXPECT_EQ(syncs, 0);
  for (std::uint64_t record = 1; record <= 10; ++record) {
    journal->append(static_cast<Timestamp>(record), "test", "{}");
    const auto recovered = FileJournal::read(file.path, journal->head());
    EXPECT_EQ(recovered.records.size(), record);
    EXPECT_FALSE(recovered.truncated_final_line);
    EXPECT_EQ(syncs, 1);  // The first append always syncs.
  }
  journal->flush();
  EXPECT_EQ(syncs, 2);
  journal->flush();
  journal.reset();
  EXPECT_EQ(syncs, 2);
}

TEST(TradingJournal, BatchedIntervalUsesSteadyTimeAndFlushRestartsIt) {
  TemporaryJournal file;
  int syncs = 0;
  auto now = std::chrono::steady_clock::time_point{};
  FileJournal::Options options;
  options.sync_policy = FileJournal::SyncPolicy::Batched;
  options.hooks.sync = [&](int) { ++syncs; return true; };
  options.hooks.clock = [&] { return now; };
  auto journal = FileJournal::create(file.path, options);
  journal->append(0, "first", "{}");
  now += std::chrono::milliseconds(249);
  journal->append(md::kNanosPerDay, "market_time_does_not_sync", "{}");
  EXPECT_EQ(syncs, 1);
  now += std::chrono::milliseconds(1);
  journal->append(md::kNanosPerDay, "interval", "{}");
  EXPECT_EQ(syncs, 2);
  now += std::chrono::milliseconds(100);
  journal->append(md::kNanosPerDay, "pending", "{}");
  journal->flush();
  EXPECT_EQ(syncs, 3);
  now += std::chrono::milliseconds(249);
  journal->append(md::kNanosPerDay, "pending", "{}");
  EXPECT_EQ(syncs, 3);
  now += std::chrono::milliseconds(1);
  journal->append(md::kNanosPerDay, "interval", "{}");
  EXPECT_EQ(syncs, 4);
}

TEST(TradingJournal, DestructionFlushesPendingRecordsAndNeverThrows) {
  for (const bool fail : {false, true}) {
    TemporaryJournal file;
    int syncs = 0;
    FileJournal::Options options;
    options.sync_policy = FileJournal::SyncPolicy::Batched;
    options.hooks.clock = [] { return std::chrono::steady_clock::time_point{}; };
    options.hooks.sync = [&](int) { return ++syncs == 1 || !fail; };
    {
      const auto journal = FileJournal::create(file.path, options);
      journal->append(0, "first", "{}");
      journal->append(1, "pending", "{}");
      EXPECT_EQ(syncs, 1);
    }
    EXPECT_EQ(syncs, 2);
    EXPECT_EQ(FileJournal::read(file.path).records.size(), 2U);
    EXPECT_NO_THROW(FileJournal::resume(file.path));  // Destruction also releases the lock.
  }
}

TEST(TradingJournal, DeferredSyncFailureIsIndeterminateAndLatchesAppendFailure) {
  for (const bool explicit_flush : {false, true}) {
    TemporaryJournal file;
    int syncs = 0;
    auto now = std::chrono::steady_clock::time_point{};
    FileJournal::Options options;
    options.sync_policy = FileJournal::SyncPolicy::Batched;
    options.hooks.clock = [&] { return now; };
    options.hooks.sync = [&](int) { return ++syncs == 1; };
    const auto journal = FileJournal::create(file.path, options);
    journal->append(0, "first", "{}");
    journal->append(1, "pending", "{}");
    const auto head = journal->head();
    now += options.sync_interval;
    try {
      if (explicit_flush) journal->flush();
      else journal->append(2, "indeterminate", "{}");
      FAIL() << "Deferred sync must fail";
    } catch (const TradingError& error) { EXPECT_EQ(error.code(), Reason::JOURNAL_IO); }
    EXPECT_EQ(journal->sequence(), 2U);
    EXPECT_EQ(journal->head(), head);
    const auto written = file.read();
    EXPECT_EQ(FileJournal::read(file.path).records.size(), explicit_flush ? 2U : 3U);
    try {
      journal->append(3, "must_not_write", "{}");
      FAIL() << "Failed journal must stay failed";
    } catch (const TradingError& error) { EXPECT_EQ(error.code(), Reason::JOURNAL_IO); }
    EXPECT_THROW(journal->flush(), TradingError);
    EXPECT_EQ(syncs, 2);
    EXPECT_EQ(file.read(), written);
  }
}

TEST(TradingJournal, ResumeSyncsItsFirstAppendAndDefaultsToPerRecord) {
  TemporaryJournal file;
  { const auto journal = FileJournal::create(file.path); journal->append(0, "older", "{}"); }
  int syncs = 0;
  FileJournal::Options options;
  options.hooks.sync = [&](int) { ++syncs; return true; };
  options.hooks.clock = [] { return std::chrono::steady_clock::time_point{}; };
  {
    const auto journal = FileJournal::resume(file.path, options);
    journal->append(1, "default", "{}");
    journal->append(2, "default", "{}");
    EXPECT_EQ(syncs, 2);
  }
  options.sync_policy = FileJournal::SyncPolicy::Batched;
  {
    const auto journal = FileJournal::resume(file.path, options);
    journal->append(3, "first_after_resume", "{}");
    EXPECT_EQ(syncs, 3);
    journal->append(4, "pending", "{}");
    EXPECT_EQ(syncs, 3);
  }
  EXPECT_EQ(syncs, 4);
  EXPECT_EQ(FileJournal::read(file.path).records.size(), 5U);
}

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
// The first field of `expected` that `actual` lacks or holds another value in, or ""
// when it has them all. Later builds add state fields, so an older snapshot is a subset.
std::string mismatch(const nlohmann::json& actual, const nlohmann::json& expected, const std::string& path = "") {
  if (expected.is_object()) {
    if (!actual.is_object()) return path + " (not an object)";
    for (const auto& [key, value] : expected.items()) {
      if (!actual.contains(key)) return path + "/" + key + " (missing)";
      if (auto where = mismatch(actual.at(key), value, path + "/" + key); !where.empty()) return where;
    }
    return "";
  }
  if (expected.is_array()) {
    if (!actual.is_array() || actual.size() != expected.size()) return path + " (array size)";
    for (std::size_t i = 0; i < expected.size(); ++i)
      if (auto where = mismatch(actual[i], expected[i], path + "/" + std::to_string(i)); !where.empty()) return where;
    return "";
  }
  // Derived analytics (scenario P&L, Greeks) go through libm, whose last bits differ
  // between platforms; recorded money is integer and compares exactly.
  if (expected.is_number_float() && actual.is_number()) {
    const double a = actual.get<double>(), e = expected.get<double>();
    if (std::abs(a - e) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(e)})) return "";
  }
  return actual == expected ? "" : path + ": " + actual.dump() + " != " + expected.dump();
}
// Captured with the original f31e910 binary: daily loss cancelled both bracket
// exits and a manual close, then rejected another close. Recovery preserves it.
TEST(TradingJournal, PreReduceOnlyKillJournalRecoversItsOriginalState) {
  const auto directory = std::filesystem::path(OPENPORT_TEST_DATA_DIR);
  const auto recovery = FileJournal::read((directory / "kill-before-reduce-only.jsonl").string());
  auto restored = TradingSession::recover(recovery);
  std::ifstream expected(directory / "kill-before-reduce-only.snapshot.json");
  std::string snapshot;
  std::getline(expected, snapshot);
  ASSERT_FALSE(snapshot.empty());
  EXPECT_EQ(mismatch(nlohmann::json::parse(restored.snapshot_json()), nlohmann::json(nlohmann::json::parse(snapshot).get<TradingSnapshot>())), "");
  EXPECT_TRUE(restored.snapshot()->risk.kill_latched);
  EXPECT_TRUE(restored.snapshot()->open_orders.empty());
  ASSERT_EQ(restored.snapshot()->positions.size(), 1U);
  EXPECT_EQ(restored.snapshot()->recent_orders.back().reason.code, Reason::KILL_SWITCH);
}

TEST(TradingJournal, IdleEmptyBatchesAreNotRecordedButTimeRulesStillRun) {
  test::ScriptedMarket f;
  auto journal = std::make_shared<FailingJournal>();
  TradingSession s({}, 0, journal);
  EXPECT_EQ(journal->count, 1);  // session_start
  // The first real market time starts the attempt, so it is recorded.
  s.on_quotes({}, {}, f.time);
  EXPECT_EQ(journal->count, 2);
  EXPECT_EQ(s.snapshot()->evaluation.started, f.time);
  // Flat with no open orders, later empty batches change nothing but the clock.
  const auto version = s.snapshot()->account_version;
  for (int i = 1; i <= 50; ++i) EXPECT_EQ(s.on_quotes({}, {}, f.time + i * md::kNanosPerSecond).account_version, version);
  EXPECT_EQ(journal->count, 2);
  EXPECT_EQ(s.snapshot()->time, f.time);
  // Time must still move forward.
  EXPECT_THROW(s.on_quotes({}, {}, f.time - 1), TradingError);
  // A working order makes time matter again: empty batches are recorded, and
  // a DAY order still ends with the session.
  f.next();
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("rest", 1, "4.00"), f.time).decision.ok());
  const auto working = journal->count;
  s.on_quotes({}, {}, f.time + md::kNanosPerSecond);
  EXPECT_EQ(journal->count, working + 1);
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 9, 22}, 16, 20));
  EXPECT_EQ(s.snapshot()->recent_orders.back().reason.code, Reason::DAY_END);
  // Flat again, so idle again.
  const auto ended = journal->count;
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 9, 22}, 17, 0));
  EXPECT_EQ(journal->count, ended);
}

TEST(TradingJournal, EmptyBatchesAreRecordedWhileAPositionIsHeld) {
  test::ScriptedMarket f;
  auto journal = std::make_shared<FailingJournal>();
  TradingSession s({}, f.time, journal);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 1), f.time).decision.ok());
  const auto held = journal->count;
  s.on_quotes({}, {}, f.time + 2 * md::kNanosPerMinute);
  EXPECT_EQ(journal->count, held + 1);
  // Its mark ages, and the snapshot says so.
  EXPECT_FALSE(s.snapshot()->valuation_complete);
}

TEST(TradingJournal, BatchesThatChangeNothingAreNotRecorded) {
  test::ScriptedMarket f;
  auto journal = std::make_shared<FailingJournal>();
  TradingSession s({}, f.time, journal);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 1), f.time).decision.ok());
  const auto held = journal->count;
  const auto version = s.snapshot()->account_version;
  // The same market time and the same quote again, as batches of other news bring.
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(s.on_quotes({}, {}, f.time).account_version, version);
    EXPECT_EQ(s.on_quotes({f.quote()}, {f.valuation()}, f.time).account_version, version);
  }
  EXPECT_EQ(journal->count, held);
  // A later market time is a change.
  s.on_quotes({}, {}, f.time + md::kNanosPerSecond);
  EXPECT_EQ(journal->count, held + 1);
  EXPECT_EQ(s.snapshot()->account_version, version + 1);
}

TEST(TradingJournal, SkippedIdleBatchesRecoverToTheSameState) {
  TemporaryJournal file;
  test::ScriptedMarket f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(file.path);
    TradingSession s({}, f.time, journal);
    f.seed(s);
    for (int i = 1; i <= 20; ++i) s.on_quotes({}, {}, f.time + i * md::kNanosPerSecond);
    expected = s.snapshot_json();
    head = journal->head();
  }
  EXPECT_EQ(FileJournal::read(file.path).records.size(), 3);  // session_start, definition, market
  auto s = TradingSession::recover(FileJournal::read(file.path, head), FileJournal::resume(file.path));
  EXPECT_EQ(s.snapshot_json(), expected);
  // Commands carry on from the recorded clock.
  ASSERT_TRUE(s.submit(f.market("after", 1), f.time + 30 * md::kNanosPerSecond).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Filled);
}

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
TEST(TradingJournal, RepairCutsATornFinalLineAfterKeepingTheOriginal) {
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
  const std::string torn = "{\"seq\":4,\"ti";  // where the disk ran out
  file.write(good + torn);
  EXPECT_THROW(FileJournal::resume(file.path), TradingError);
  const auto repaired = FileJournal::repair(file.path);
  EXPECT_EQ(repaired.bytes_cut, torn.size());
  ASSERT_FALSE(repaired.backup.empty());
  std::ifstream backup(repaired.backup, std::ios::binary);
  std::ostringstream kept;
  kept << backup.rdbuf();
  EXPECT_EQ(kept.str(), good + torn);
  EXPECT_EQ(file.read(), good);
  // It resumes and recovers as it stood, and a whole journal is left alone.
  auto resumed = FileJournal::resume(file.path);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path), resumed).snapshot_json(), snapshot);
  resumed.reset();
  EXPECT_EQ(FileJournal::repair(file.path).bytes_cut, 0U);
  EXPECT_EQ(file.read(), good);
  std::filesystem::remove(repaired.backup);
}
TEST(TradingJournal, RepairLeavesDamageBeforeTheLastLineAndLiveJournalsAlone) {
  TemporaryJournal file;
  test::ScriptedMarket f;
  {
    auto journal = FileJournal::create(file.path);
    TradingSession s({}, f.time, journal);
    f.seed(s);
  }
  auto damaged = file.read();
  damaged[damaged.find("\"time\":") + 8] ^= 1;  // a changed digit breaks the hash chain
  file.write(damaged + "{\"seq\":4,");
  try {
    (void)FileJournal::repair(file.path);
    ADD_FAILURE() << "a damaged journal must not be repaired";
  } catch (const TradingError& error) {
    EXPECT_EQ(error.code(), Reason::JOURNAL_CORRUPT);
  }
  EXPECT_EQ(file.read(), damaged + "{\"seq\":4,");
  // A journal a server has open is locked against repair.
  file.write("");
  auto live = FileJournal::create(file.path + ".live");
  try {
    (void)FileJournal::repair(file.path + ".live");
    ADD_FAILURE() << "a journal in use must not be repaired";
  } catch (const TradingError& error) {
    EXPECT_EQ(error.code(), Reason::JOURNAL_LOCKED);
  }
  live.reset();
  std::filesystem::remove(file.path + ".live");
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
TEST(TradingJournal, LockRejectsSeparateOpensAndIsReleasedOnDestruction) {
  TemporaryJournal file;
  const auto message = "paper journal '" + file.path +
      "' is in use by another openportd; use --paper-journal to choose another file or --no-paper";
  auto expect_locked = [&](bool create) {
    try {
      const auto second = create ? FileJournal::create(file.path) : FileJournal::resume(file.path);
      FAIL() << "A second writer acquired the journal";
    } catch (const TradingError& error) {
      EXPECT_EQ(error.code(), Reason::JOURNAL_LOCKED);
      EXPECT_EQ(error.what(), message);
    }
  };
  {
    auto journal = FileJournal::create(file.path);
    journal->append(1, "first", "{}");
    const auto before = file.read();
    expect_locked(true);
    expect_locked(false);
    EXPECT_EQ(file.read(), before);
    // Closing a separate reader must not release the writer's lock.
    EXPECT_EQ(FileJournal::read(file.path).records.size(), 1u);
    expect_locked(false);
    journal->append(2, "second", "{}");
    EXPECT_EQ(FileJournal::read(file.path).records.size(), 2u);
  }
  {
    auto resumed = FileJournal::resume(file.path);
    EXPECT_EQ(resumed->sequence(), 2u);
    expect_locked(false);
    resumed->append(3, "third", "{}");
  }
  auto later = FileJournal::resume(file.path);
  EXPECT_EQ(later->sequence(), 3u);
}
TEST(TradingJournal, LockedJournalIsRejectedBeforeReadingOrRepairingItsContents) {
  TemporaryJournal file;
  const auto journal = FileJournal::create(file.path);
  file.write("invalid journal contents\n");
  try {
    const auto second = FileJournal::resume(file.path);
    FAIL() << "A second writer acquired the journal";
  } catch (const TradingError& error) {
    EXPECT_EQ(error.code(), Reason::JOURNAL_LOCKED);
  }
  EXPECT_EQ(file.read(), "invalid journal contents\n");
}
}  // namespace
}  // namespace openport::trading

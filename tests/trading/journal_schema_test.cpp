#include <filesystem>
#include <random>
#include <tuple>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "support/scripted_market.hpp"
#include "trading/state_delta.hpp"

namespace openport::trading {
namespace {
using Json = nlohmann::json;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-schema-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
    path = pattern;
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
  std::string file(std::string_view name) const { return (path / name).string(); }
  std::filesystem::path path;
};
class MemoryJournal final : public Journal {
 public:
  struct Entry { Timestamp time; std::string type; Json payload; };
  std::vector<Entry> entries;
  void append(Timestamp time, std::string_view type, std::string_view payload) override {
    entries.push_back({time, std::string(type), Json::parse(payload)});
  }
  std::uint64_t sequence() const override { return entries.size(); }
  std::string head() const override { return std::string(64, '0'); }
};
Json patched(Json state, const Json& delta) {
  detail::apply_state_delta(state, delta);
  return state;
}
void expect_round_trip(const Json& before, const Json& after) {
  const auto delta = detail::state_delta(before, after);
  EXPECT_EQ(patched(before, delta), after) << delta.dump();
  // Through text, as the journal stores it.
  EXPECT_EQ(patched(Json::parse(before.dump()), Json::parse(delta.dump())), after) << delta.dump();
}
void expect_corrupt(const std::function<void()>& action) {
  try {
    action();
    ADD_FAILURE() << "expected JOURNAL_CORRUPT";
  } catch (const TradingError& error) { EXPECT_EQ(error.code(), Reason::JOURNAL_CORRUPT) << error.what(); }
}
/// Rewrite a journal's payloads into a new file, as a damaged or foreign writer might.
std::string rewritten(const TemporaryDirectory& directory, std::string_view name, const std::string& source,
                      const std::function<void(std::size_t, Json&)>& change) {
  const auto path = directory.file(name);
  auto out = FileJournal::create(path);
  const auto recovery = FileJournal::read(source);
  for (std::size_t i = 0; i < recovery.records.size(); ++i) {
    auto payload = Json::parse(recovery.records[i].payload);
    change(i, payload);
    out->append(recovery.records[i].time, recovery.records[i].type, payload.dump());
  }
  return path;
}
/// A day of round trips and cancelled orders, so the history grows.
std::string trade(TradingSession& s, test::ScriptedMarket& f, int rounds) {
  for (int i = 0; i < rounds; ++i) {
    f.next();
    s.on_quotes({f.quote()}, {f.valuation()}, f.time);
    EXPECT_TRUE(s.submit(f.market("buy-" + std::to_string(i)), f.time).decision.ok());
    EXPECT_TRUE(s.submit(f.market("sell-" + std::to_string(i), 1, Side::Sell), f.time).decision.ok());
    const auto rest = s.submit(f.limit("rest-" + std::to_string(i), 1, "3.90"), f.time);
    EXPECT_TRUE(rest.decision.ok());
    s.cancel(*rest.order_id, f.time);
  }
  return s.snapshot_json();
}

TEST(StateDelta, RecordsOnlyWhatChanged) {
  const Json before = {{"cash", 100}, {"orders", Json::array({{{"id", 1}, {"status", "working"}}, {{"id", 2}, {"status", "filled"}}})},
                       {"books", {{"A", {{"bid", 1.5}, {"ask", 1.75}}}, {"B", {{"bid", 2}, {"ask", 2.25}}}}}, {"kill", false}};
  auto after = before;
  after["books"]["A"]["bid"] = 1.55;
  after["orders"][0]["status"] = "filled";
  after["orders"].push_back({{"id", 3}, {"status", "working"}});
  after["books"].erase("B");
  after["kill_reason"] = "";
  const auto delta = detail::state_delta(before, after);
  EXPECT_EQ(delta, Json::parse(R"({"o":{"books":{"d":["B"],"o":{"A":{"o":{"bid":{"v":1.55}}}}},"kill_reason":{"v":""},)"
                               R"("orders":{"a":{"0":{"o":{"status":{"v":"filled"}}},"2":{"v":{"id":3,"status":"working"}}},"n":3}}})"));
  expect_round_trip(before, after);
  EXPECT_EQ(detail::state_delta(before, before), Json::parse(R"({"o":{}})"));
}

TEST(StateDelta, RewrittenArraysAndChangedTypesAreRecordedWhole) {
  const Json settled = Json::array({"A", "C", "D"});
  EXPECT_EQ(detail::state_delta(settled, Json::array({"A", "B", "C", "D"})), (Json{{"v", {"A", "B", "C", "D"}}}));
  EXPECT_EQ(detail::state_delta(Json::object(), Json::array()), (Json{{"v", Json::array()}}));
  EXPECT_EQ(detail::state_delta(1, "one"), (Json{{"v", "one"}}));
  expect_round_trip(settled, Json::array({"A"}));
  expect_round_trip(Json::array(), Json::array({1, 2}));
  expect_round_trip(Json::array({1, 2, 3, 4}), Json::array({1, 2}));
  expect_round_trip({{"a", {{"b", Json::array({1, {{"c", nullptr}}})}}}}, {{"a", {{"b", Json::array({1, {{"c", 2}}, 3})}}}});
}

Json random_value(std::mt19937& random, int depth) {
  switch (random() % (depth > 3 ? 4 : 6)) {
    case 0: return static_cast<std::int64_t>(random() % 2000) - 1000;
    case 1: return static_cast<double>(random() % 100000) / 7.0;
    case 2: return std::string(1 + random() % 3, static_cast<char>('a' + random() % 4));
    case 3: return random() % 2 == 0;
    case 4: {
      Json array = Json::array();
      for (auto n = random() % 5; n > 0; --n) array.push_back(random_value(random, depth + 1));
      return array;
    }
    default: {
      Json object = Json::object();
      for (auto n = random() % 5; n > 0; --n) object[std::string(1, static_cast<char>('a' + random() % 6))] = random_value(random, depth + 1);
      return object;
    }
  }
}
void mutate(Json& value, std::mt19937& random, int depth) {
  if (value.is_object() && !value.empty() && random() % 4 != 0) {
    auto it = value.begin();
    std::advance(it, random() % value.size());
    if (random() % 5 == 0) value.erase(it);
    else mutate(it.value(), random, depth + 1);
  } else if (value.is_array() && !value.empty() && random() % 4 != 0) {
    const auto choice = random() % 6;
    if (choice == 0) value.push_back(random_value(random, depth + 1));
    else if (choice == 1) value.erase(value.end() - 1);
    else mutate(value[random() % value.size()], random, depth + 1);
  } else {
    value = random_value(random, depth);
  }
}

TEST(StateDelta, AnyChangeRoundTrips) {
  std::mt19937 random(20260923);
  for (int trial = 0; trial < 2000; ++trial) {
    const auto before = random_value(random, 0);
    auto after = before;
    for (auto n = 1 + random() % 4; n > 0; --n) mutate(after, random, 0);
    expect_round_trip(before, after);
  }
}

TEST(StateDelta, DeltasThatDoNotFitAreRejected) {
  Json state = {{"orders", Json::array({1, 2})}, {"cash", 5}};
  for (const auto* delta : {R"({"o":{"cash":{"o":{}}}})", R"({"o":{"orders":{"a":{"2":{"v":3}},"n":2}}})",
                            R"({"o":{"missing":{"o":{}}}})", R"({"o":{"orders":{"a":{"2":{"o":{}}},"n":3}}})",
                            R"({"o":{"orders":{"a":{"x":{"v":3}},"n":2}}})", R"({"x":1})", R"([1])"}) {
    auto copy = state;
    EXPECT_THROW(detail::apply_state_delta(copy, Json::parse(delta)), std::invalid_argument) << delta;
  }
}

TEST(TradingJournalSchema, OptionalExecutionRulesRoundTripAndOlderRulesKeepTheirDefaults) {
  TemporaryDirectory directory;
  test::ScriptedMarket f;
  const auto source = directory.file("rules.jsonl");
  std::string expected;
  {
    SessionConfig config;
    config.rules.slippage_ticks = 2;
    config.rules.margin = MarginMode::Portfolio;
    TradingSession s(config, f.time, FileJournal::create(source));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("buy"), f.time).decision.ok());
    expected = s.snapshot_json();
  }
  auto recovered = TradingSession::recover(FileJournal::read(source));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  EXPECT_EQ(recovered.config().rules.slippage_ticks, 2);
  EXPECT_EQ(recovered.config().rules.margin, MarginMode::Portfolio);
  EXPECT_EQ(recovered.snapshot()->recent_fills.front().price, Money::parse("4.40"));
  f.next();
  recovered.on_quotes({f.quote()}, {f.valuation()}, f.time);
  ASSERT_TRUE(recovered.submit(f.market("sell", 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(recovered.snapshot()->recent_fills.back().price, Money::parse("3.80"));

  const auto defaults = directory.file("defaults.jsonl");
  { TradingSession s({}, f.time, FileJournal::create(defaults)); }
  const auto legacy = rewritten(directory, "legacy.jsonl", defaults, [](std::size_t, Json& payload) {
    auto& rules = payload["state"]["config"]["rules"];
    rules.erase("slippage_ticks");
    rules.erase("margin");
    for (auto& event : payload["events"]) {
      auto& event_rules = event["payload"]["rules"];
      event_rules.erase("slippage_ticks");
      event_rules.erase("margin");
    }
  });
  const auto older = TradingSession::recover(FileJournal::read(legacy));
  EXPECT_EQ(older.config().rules.slippage_ticks, 0);
  EXPECT_EQ(older.config().rules.margin, MarginMode::Strategy);
  EXPECT_EQ(older.snapshot_json(), TradingSession::recover(FileJournal::read(defaults)).snapshot_json());
  expect_corrupt([&] {
    const auto bad = rewritten(directory, "bad-margin.jsonl", defaults, [](std::size_t, Json& payload) {
      payload["state"]["config"]["rules"]["margin"] = "unknown";
    });
    (void)TradingSession::recover(FileJournal::read(bad));
  });
  for (const auto& value : {Json(-1), Json(11), Json(1.5), Json(true), Json(nullptr)}) {
    expect_corrupt([&] {
      const auto bad = rewritten(directory, "bad-slippage-" + value.dump() + ".jsonl", defaults, [&](std::size_t, Json& payload) {
        payload["state"]["config"]["rules"]["slippage_ticks"] = value;
      });
      (void)TradingSession::recover(FileJournal::read(bad));
    });
  }
}

TEST(TradingJournalSchema, RecordsCarryTheChangeAndStaySmallAsHistoryGrows) {
  TemporaryDirectory directory;
  const auto path = directory.file("account.jsonl");
  test::ScriptedMarket f;
  std::string expected;
  {
    TradingSession s({}, f.time, FileJournal::create(path));
    f.seed(s);
    expected = trade(s, f, 60);
  }
  const auto recovery = FileJournal::read(path);
  const auto first = Json::parse(recovery.records.front().payload);
  EXPECT_EQ(first.at("schema"), 3);
  EXPECT_TRUE(first.contains("state"));
  std::size_t deltas = 0;
  for (const auto& record : recovery.records) {
    const auto payload = Json::parse(record.payload);
    EXPECT_FALSE(payload.contains("snapshot"));
    EXPECT_NE(payload.contains("state"), payload.contains("delta"));
    deltas += payload.contains("delta");
  }
  EXPECT_GT(deltas, recovery.records.size() * 9 / 10);
  // A fill's record costs the same after 60 round trips as after 5, and a
  // small part of the whole state that schema 2 wrote each time.
  MemoryJournal whole;
  TradingSession::expand(recovery, whole);
  auto index_of = [&](std::string_view client) {
    const auto needle = "\"client_order_id\":\"" + std::string(client) + "\"";
    for (std::size_t i = 0; i < recovery.records.size(); ++i)
      if (recovery.records[i].type == "submit" && recovery.records[i].payload.find(needle) != std::string::npos) return i;
    ADD_FAILURE() << "no record for " << client;
    return std::size_t{0};
  };
  const auto early = index_of("sell-5");
  const auto late = index_of("sell-59");
  EXPECT_LT(recovery.records[late].payload.size(), recovery.records[early].payload.size() + 200);
  EXPECT_LT(recovery.records[late].payload.size() * 20, whole.entries[late].payload.dump().size());
  EXPECT_EQ(TradingSession::recover(recovery).snapshot_json(), expected);
}

TEST(TradingJournalSchema, CheckpointsComeAtLeastEveryThousandRecords) {
  test::ScriptedMarket f;
  auto memory = std::make_shared<MemoryJournal>();
  TradingSession s({}, f.time, memory);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("hold"), f.time).decision.ok());
  // With a position held every batch is a transaction.
  for (int i = 0; i < 2100; ++i) {
    f.next();
    s.on_quotes({f.quote(i % 2 ? "4.00" : "4.05")}, {f.valuation()}, f.time);
  }
  std::vector<std::size_t> checkpoints;
  for (std::size_t i = 0; i < memory->entries.size(); ++i)
    if (memory->entries[i].payload.contains("state")) checkpoints.push_back(i);
  ASSERT_GE(checkpoints.size(), 3U);
  EXPECT_EQ(checkpoints.front(), 0U);
  for (std::size_t i = 1; i < checkpoints.size(); ++i) EXPECT_LE(checkpoints[i] - checkpoints[i - 1], 1000U);
}

TEST(TradingJournalSchema, RecoveryRebuildsEveryStateAndTheNextRecordIsWhole) {
  TemporaryDirectory directory;
  const auto path = directory.file("account.jsonl");
  test::ScriptedMarket f;
  {
    TradingSession s({}, f.time, FileJournal::create(path));
    f.seed(s);
    trade(s, f, 5);
    ASSERT_TRUE(s.submit(f.market("hold", 2), f.time).decision.ok());
  }
  std::string expected;
  {
    auto s = TradingSession::recover(FileJournal::read(path), FileJournal::resume(path));
    f.next();
    s.on_quotes({f.quote("4.10", "4.30")}, {f.valuation()}, f.time);
    f.next();
    s.on_quotes({f.quote("4.20", "4.40")}, {f.valuation()}, f.time);
    expected = s.snapshot_json();
  }
  const auto recovery = FileJournal::read(path);
  const auto n = recovery.records.size();
  EXPECT_TRUE(Json::parse(recovery.records[n - 2].payload).contains("state"));
  EXPECT_TRUE(Json::parse(recovery.records[n - 1].payload).contains("delta"));
  EXPECT_EQ(TradingSession::recover(recovery).snapshot_json(), expected);
}

TEST(TradingJournalSchema, OlderJournalsCompactToTheSameAccount) {
  TemporaryDirectory directory;
  const auto current = directory.file("current.jsonl");
  test::ScriptedMarket f;
  std::string expected;
  {
    TradingSession s({}, f.time, FileJournal::create(current));
    f.seed(s);
    trade(s, f, 12);
    ASSERT_TRUE(s.submit(f.market("hold", 2), f.time).decision.ok());
    expected = s.snapshot_json();
  }
  // A schema 2 journal, as builds before schema 3 wrote it.
  const auto expanded = directory.file("expanded.jsonl");
  TradingSession::expand(FileJournal::read(current), *FileJournal::create(expanded));
  const auto older = FileJournal::read(expanded);
  for (const auto& record : older.records) {
    const auto payload = Json::parse(record.payload);
    EXPECT_EQ(payload.at("schema"), 2);
    EXPECT_TRUE(payload.contains("state") && payload.contains("snapshot") && !payload.contains("delta"));
  }
  EXPECT_EQ(TradingSession::recover(older).snapshot_json(), expected);

  const auto compacted = directory.file("compacted.jsonl");
  EXPECT_EQ(TradingSession::compact(older, *FileJournal::create(compacted)), expected);
  const auto rewritten = FileJournal::read(compacted);
  ASSERT_EQ(rewritten.records.size(), older.records.size());
  for (std::size_t i = 0; i < older.records.size(); ++i) {
    const auto& a = older.records[i];
    const auto& b = rewritten.records[i];
    EXPECT_EQ(std::tie(a.seq, a.time, a.type), std::tie(b.seq, b.time, b.type));
    auto was = Json::parse(a.payload);
    auto is = Json::parse(b.payload);
    EXPECT_EQ(is.at("schema"), 3);
    for (auto* payload : {&was, &is})
      for (const auto* key : {"schema", "state", "snapshot", "delta"}) payload->erase(key);
    EXPECT_EQ(was, is) << "record " << a.seq;
  }
  EXPECT_LT(std::filesystem::file_size(compacted) * 4, std::filesystem::file_size(expanded));
  EXPECT_EQ(TradingSession::recover(rewritten).snapshot_json(), expected);
  // A compacted journal compacts to itself, and a resumed older journal carries on in schema 3.
  const auto again = directory.file("again.jsonl");
  EXPECT_EQ(TradingSession::compact(rewritten, *FileJournal::create(again)), expected);
  EXPECT_EQ(FileJournal::read(again).head, rewritten.head);
  auto s = TradingSession::recover(older, FileJournal::resume(expanded));
  f.next();
  s.on_quotes({f.quote("4.30", "4.50")}, {f.valuation()}, f.time);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(expanded)).snapshot_json(), s.snapshot_json());
  EXPECT_THROW(TradingSession::compact(older, *FileJournal::resume(again)), TradingError);
}

TEST(TradingJournalSchema, DamagedDeltasAreCorrupt) {
  TemporaryDirectory directory;
  const auto source = directory.file("source.jsonl");
  test::ScriptedMarket f;
  {
    TradingSession s({}, f.time, FileJournal::create(source));
    f.seed(s);
    trade(s, f, 2);
  }
  auto recover = [](const std::string& path) { (void)TradingSession::recover(FileJournal::read(path)); };
  // The first delta after the opening checkpoint.
  std::size_t d = 0;
  const auto records = FileJournal::read(source).records;
  while (d < records.size() && !Json::parse(records[d].payload).contains("delta")) ++d;
  ASSERT_LT(d, records.size());
  expect_corrupt([&] {
    recover(rewritten(directory, "headless.jsonl", source, [](std::size_t i, Json& p) {
      if (i == 0) { p["delta"] = {{"o", Json::object()}}; p.erase("state"); }
    }));
  });
  expect_corrupt([&] {
    recover(rewritten(directory, "both.jsonl", source, [d](std::size_t i, Json& p) {
      if (i == d) p["state"] = Json::object();
    }));
  });
  expect_corrupt([&] {
    recover(rewritten(directory, "misfit.jsonl", source, [d](std::size_t i, Json& p) {
      if (i == d) p["delta"]["o"]["orders"] = {{"a", {{"9", {{"o", Json::object()}}}}}, {"n", 10}};
    }));
  });
  expect_corrupt([&] {
    recover(rewritten(directory, "version.jsonl", source, [d](std::size_t i, Json& p) {
      if (i == d) p["delta"]["o"]["version"] = {{"v", 99}};
    }));
  });
  expect_corrupt([&] {
    recover(rewritten(directory, "unknown.jsonl", source, [d](std::size_t i, Json& p) {
      if (i == d) p["schema"] = 4;
    }));
  });
}

}  // namespace
}  // namespace openport::trading

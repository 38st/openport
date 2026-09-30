#include "support/contract_capture.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <future>
#include <sstream>
#include <gtest/gtest.h>

#include "openport/providers/replay.hpp"
#include "openport/server/desk.hpp"
#include "openport/server/engine.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/server/run.hpp"
#include "openport/server/plans.hpp"
#include "support/recording.hpp"
#include "support/scripted_market.hpp"
#include "server/run_json.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;
using namespace std::chrono_literals;

std::string read_file(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}
std::vector<md::Event> market_batch(const test::ScriptedMarket& market, double ask = 4.2) {
  return {md::ContractDefinition{0, market.contract},
          md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000},
          md::OptionQuote{0, market.time, ask - .2, ask, 20, 20},
          md::SnapshotComplete{"SPX", market.time}};
}
server::TradingReply command(server::Desk& desk, server::TradingCommand request, md::Timestamp time, md::Timestamp driver) {
  std::optional<server::TradingReply> result;
  desk.command(std::move(request), [&](server::TradingReply reply) { result = std::move(reply); }, time, driver);
  if (!result) throw std::runtime_error("Desk did not complete command");
  return *result;
}
TEST(Desk, NamedAccountsListInIdOrderBeforeAndAfterARestart) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  server::Desk::Options options;
  options.analytics.fallback_rate = 0;
  options.paper_journal = file.directory / "paper-journal.jsonl";
  options.paper_accounts = file.directory / "accounts";
  const auto ids = [](const server::Desk& desk) {
    std::vector<std::string> result;
    for (const auto& account : desk.accounts()) result.push_back(account.id);
    return result;
  };
  // "swing-50k-2.jsonl" sorts before "swing-50k.jsonl"; an account created later can sort earlier.
  const std::vector<std::string> expected{"main", "alpha", "swing-50k", "swing-50k-2"};
  {
    server::Desk desk("test", {}, {{"SPX"}}, options);
    desk.start_trading();
    for (const auto* name : {"Swing 50k", "Swing 50k", "Alpha"}) {
      server::TradingCommand create;
      create.kind = server::TradingCommand::Kind::CreateAccount;
      create.name = name;
      create.rules = server::find_plan("practice")->rules;
      create.initial_cash = server::find_plan("practice")->initial_cash;
      const auto reply = command(desk, create, market.time, market.time);
      ASSERT_FALSE(reply.account.empty()) << reply.decision.message;
    }
    EXPECT_EQ(ids(desk), expected);
    desk.stop();
  }
  server::Desk restarted("test", {}, {{"SPX"}}, options);
  restarted.start_trading();
  EXPECT_EQ(ids(restarted), expected);
  restarted.stop();
}

TEST(Desk, OnlyExplicitReplayJournalsBatchSyncs) {
  for (const bool replay : {false, true}) {
    test::RecordingFile file;
    test::ScriptedMarket market;
    int syncs = 0;
    server::Desk::Options options;
    options.replay = replay;
    options.analytics.fallback_rate = 0;
    options.paper_journal = file.directory / "account.jsonl";
    options.journal_io.sync = [&](int) { ++syncs; return true; };
    options.journal_io.clock = [] { return std::chrono::steady_clock::time_point{}; };
    server::Desk desk("test", {}, {{"SPX"}}, options);
    desk.start_trading();
    desk.replay_batch(market_batch(market), market.time);
    server::TradingCommand order;
    order.order = market.market("sync-policy");
    const auto reply = command(desk, order, market.time, market.time);
    ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
    const auto records = trading::FileJournal::read(options.paper_journal.string()).records.size();
    ASSERT_GT(records, 1U);
    EXPECT_EQ(static_cast<std::size_t>(syncs), replay ? 1U : records);
    desk.stop();
    EXPECT_EQ(static_cast<std::size_t>(syncs), replay ? 2U : records);
    EXPECT_NO_THROW(trading::FileJournal::resume(options.paper_journal.string()));
  }
}

TEST(Desk, FailedBoundarySyncDisablesTradingAndPreservesThePublishedAccount) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  bool fail = false;
  server::Desk::Options options;
  options.replay = true;
  options.paper_journal = file.directory / "account.jsonl";
  options.journal_io.sync = [&](int) { return !fail; };
  options.journal_io.clock = [] { return std::chrono::steady_clock::time_point{}; };
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  desk.replay_batch(market_batch(market), market.time);
  const auto before = desk.trading_view()->snapshot;
  fail = true;
  desk.flush_journals();
  EXPECT_NE(desk.trading_status().reason.find("JOURNAL_IO"), std::string::npos);
  EXPECT_EQ(desk.trading_status().write, "disabled");
  server::TradingCommand order;
  order.order = market.market("failed-sync");
  const auto reply = command(desk, order, market.time, market.time);
  EXPECT_EQ(reply.error_code, "TRADING_UNAVAILABLE");
  EXPECT_EQ(desk.trading_view()->snapshot->account_version, before->account_version);
  EXPECT_TRUE(desk.trading_view()->snapshot->recent_fills.empty());
}

TEST(Desk, ExplicitClockControlsFeedStallWithoutThreads) {
  test::ScriptedMarket market;
  server::Desk::Options options;
  options.analytics.fallback_rate = 0;
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  desk.replay_batch(market_batch(market), market.time);
  server::TradingCommand order;
  order.order = market.market("stale");
  auto result = command(desk, order, market.time, market.time + 10 * md::kNanosPerMinute);
  EXPECT_EQ(result.decision.code, trading::Reason::FEED_STALLED);
  order.order.client_order_id = "fresh";
  result = command(desk, order, market.time, market.time);
  EXPECT_TRUE(result.decision.ok()) << result.decision.message;
  ASSERT_EQ(result.view->snapshot->recent_fills.size(), 1U);
  EXPECT_EQ(result.view->snapshot->recent_fills.front().price, Money::parse("4.20"));
}
TEST(Desk, SnapshotsDoNotAnalyseOtherUnderlyingsBeforeTheirCompleteBoundary) {
  test::ScriptedMarket market;
  auto spy = *md::parse_osi("SPY261022C00500000");
  server::Desk::Options options;
  options.replay = true;
  server::Desk desk("test", {}, {{"SPX", "SPY"}}, options);
  desk.start_trading();
  auto batch = market_batch(market);
  batch.insert(batch.begin(), md::ContractDefinition{1, spy});
  batch.insert(batch.begin(), md::UnderlyingQuote{"SPY", market.time, 500, 500, 500});
  desk.replay_batch(batch, market.time);
  EXPECT_FALSE(desk.metrics("SPY"));
  desk.replay_batch({md::OptionQuote{1, market.time, 4, 4.2, 20, 20}, md::SnapshotComplete{"SPY", market.time}}, market.time);
  ASSERT_TRUE(desk.metrics("SPY"));
  EXPECT_EQ(desk.metrics("SPY")->options_priced, 1);
}

TEST(Desk, AnalyticsInputsAndEquityHooksUseExplicitMarketTime) {
  test::ScriptedMarket market;
  std::vector<server::EquitySample> samples;
  server::Desk::Options options;
  options.analytics.fallback_rate = 0;
  options.equity_sample = [&](std::string_view account, const server::EquitySample& sample) {
    EXPECT_EQ(account, "main");
    samples.push_back(sample);
  };
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  desk.replay_batch(market_batch(market), market.time);
  const auto analytics = desk.metrics("SPX");
  ASSERT_TRUE(analytics);
  EXPECT_THROW(desk.apply_analytics(analytics, market.time + 1), std::invalid_argument);
  desk.apply_analytics(analytics, market.time);
  server::TradingCommand order;
  order.order = market.market("sample");
  const auto reply = command(desk, order, market.time, market.time);
  ASSERT_TRUE(reply.decision.ok());
  ASSERT_FALSE(samples.empty());
  EXPECT_EQ(samples.back().time, market.time);
  EXPECT_EQ(samples.back().equity, reply.view->snapshot->equity);
}

/// A desk whose main account keeps its equity history beside a journal, fed one
/// SPX call's quotes at explicit market times.
struct EquityDesk {
  test::RecordingFile file;
  test::ScriptedMarket market;
  std::unique_ptr<server::Desk> desk;
  explicit EquityDesk(const trading::SessionConfig& paper) {
    server::Desk::Options options;
    options.analytics.fallback_rate = 0;
    options.paper = paper;
    options.paper.limits.aggregate = {1e9, 1e9};
    options.paper.limits.per_underlying = {1e9, 1e9};
    options.paper_journal = file.directory / "main.jsonl";
    desk = std::make_unique<server::Desk>("test", md::Capabilities{}, md::Subscription{{"SPX"}}, options);
    desk->start_trading();
  }
  void quotes(md::Timestamp time, double bid, double ask) {
    market.time = time;
    desk->replay_batch({md::ContractDefinition{0, market.contract}, md::UnderlyingQuote{"SPX", time, 5000, 5000, 5000},
                        md::OptionQuote{0, time, bid, ask, 20, 20}, md::SnapshotComplete{"SPX", time}}, time);
  }
  void buy(trading::Quantity quantity) {
    server::TradingCommand request;
    request.order = market.market("buy-" + std::to_string(market.time), quantity);
    const auto reply = command(*desk, request, market.time, market.time);
    ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  }
  std::shared_ptr<const trading::TradingSnapshot> snapshot() const { return desk->trading_view()->snapshot; }
  const std::vector<server::EquitySample>& samples() const { return desk->trading_view()->equity_samples; }
};

TEST(DeskEquity, APassedAttemptsHistoryHoldsThePassingEquityAndTomorrowsFloorStopsRising) {
  // B25: the transaction that passed also liquidated at the bid, and only the fill's
  // lower equity was stored, so the chart of a passed day never reached its target.
  // B27: once decided, an end-of-day attempt's floor no longer ratchets at rollover.
  for (const auto mode : {trading::DrawdownMode::Intraday, trading::DrawdownMode::EndOfDay}) {
    trading::SessionConfig paper;
    paper.initial_cash = Money::parse("10000");
    paper.rules.profit_target = Money::parse("100");
    paper.rules.max_drawdown = Money::parse("100");
    paper.rules.drawdown_mode = mode;
    EquityDesk f(paper);
    f.quotes(md::new_york_to_utc({2026, 9, 22}, 10, 0), 4.00, 4.20);
    f.buy(1);
    const auto pass = md::new_york_to_utc({2026, 9, 22}, 10, 0) + 15 * md::kNanosPerSecond;
    f.quotes(pass, 5.20, 5.40);
    f.quotes(md::new_york_to_utc({2026, 9, 22}, 10, 1), 5.20, 5.40);
    const auto e = f.snapshot()->evaluation;
    ASSERT_EQ(e.status, trading::EvaluationStatus::Passed);
    ASSERT_EQ(e.decided_equity, Money::parse("10109.35"));
    EXPECT_EQ(f.snapshot()->equity, Money::parse("10098.70"));  // sold at the 5.20 bid
    const auto& samples = f.samples();
    const auto passing = std::find_if(samples.begin(), samples.end(), [&](const server::EquitySample& s) {
      return s.time == pass && s.fill == 0 && s.equity == e.decided_equity;
    });
    ASSERT_NE(passing, samples.end());
    EXPECT_EQ(passing->target, Money::parse("10100"));
    std::size_t later = 0;
    for (const auto& sample : samples) {
      if (sample.time < pass) continue;
      ++later;
      if (mode == trading::DrawdownMode::EndOfDay) EXPECT_EQ(sample.tomorrow_floor, sample.floor);
      else EXPECT_FALSE(sample.tomorrow_floor);
    }
    EXPECT_GE(later, 3U);  // the passing mark, the liquidation fill and 10:01
  }
}

TEST(DeskEquity, ARolloverLiquidationIsStoredUnderTheRatchetedFloor) {
  // B26: the end-of-day ratchet and the liquidation it caused share the rollover
  // transaction, and the fill's sample carried the previous day's peak and floor.
  trading::SessionConfig paper;
  paper.initial_cash = Money::parse("10000");
  paper.rules.max_drawdown = Money::parse("100");
  paper.rules.drawdown_mode = trading::DrawdownMode::EndOfDay;
  EquityDesk f(paper);
  f.quotes(md::new_york_to_utc({2026, 9, 22}, 10, 0), 4.00, 4.20);
  f.buy(5);
  f.quotes(md::new_york_to_utc({2026, 9, 22}, 15, 59), 4.40, 4.60);  // closes at 10,146.75
  f.quotes(md::new_york_to_utc({2026, 9, 23}, 9, 30), 4.00, 4.20);
  const auto snapshot = f.snapshot();
  ASSERT_EQ(snapshot->evaluation.status, trading::EvaluationStatus::Failed);
  ASSERT_EQ(snapshot->recent_fills.size(), 2U);
  const auto& samples = f.samples();
  const auto liquidation = std::find_if(samples.begin(), samples.end(), [](const server::EquitySample& s) { return s.fill == 2; });
  ASSERT_NE(liquidation, samples.end());
  EXPECT_EQ(liquidation->peak, Money::parse("10146.75"));
  EXPECT_EQ(liquidation->floor, Money::parse("10046.75"));
  EXPECT_EQ(liquidation->tomorrow_floor, liquidation->floor);
}

TEST(DeskEquity, OnlyAPassOfThePresetItselfUnlocksItsFundedPlan) {
  // B36: custom rules that only borrowed the name "Intraday 25K" unlocked funded-intraday-25k.
  const auto* evaluation = server::find_plan("intraday-25k");
  const auto* funded = server::find_plan("funded-intraday-25k");
  ASSERT_TRUE(evaluation && funded);
  for (const bool preset : {false, true}) {
    trading::SessionConfig paper;
    paper.initial_cash = preset ? evaluation->initial_cash : Money::parse("1000000");
    paper.rules = evaluation->rules;
    if (!preset) paper.rules.profit_target = Money::parse("1");
    EquityDesk f(paper);
    f.quotes(md::new_york_to_utc({2026, 9, 22}, 10, 0), 4.00, 4.20);
    f.buy(1);
    f.quotes(md::new_york_to_utc({2026, 9, 22}, 10, 0) + 15 * md::kNanosPerSecond, preset ? 29.40 : 5.20, preset ? 29.60 : 5.40);
    ASSERT_EQ(f.snapshot()->evaluation.status, trading::EvaluationStatus::Passed);
    server::TradingCommand reset;
    reset.kind = server::TradingCommand::Kind::ResetAccount;
    reset.initial_cash = funded->initial_cash;
    reset.rules = funded->rules;
    reset.reason = "funded";
    reset.required_pass = evaluation->name;
    const auto reply = command(*f.desk, reset, f.market.time, f.market.time);
    EXPECT_EQ(reply.decision.ok(), preset) << reply.decision.message;
    if (!preset) EXPECT_EQ(reply.decision.code, trading::Reason::PLAN_LOCKED);
  }
  // The fill model's execution settings are not part of the plan.
  auto conservative = evaluation->rules;
  conservative.fill_latency_ms = 1000; conservative.slippage_ticks = 1; conservative.impact_ticks = 1;
  EXPECT_TRUE(server::follows_plan(*evaluation, evaluation->initial_cash, conservative));
  EXPECT_FALSE(server::follows_plan(*evaluation, Money::parse("1000000"), evaluation->rules));
  auto looser = evaluation->rules;
  looser.max_drawdown = Money::parse("5000");
  EXPECT_FALSE(server::follows_plan(*evaluation, evaluation->initial_cash, looser));
}

void write_stream(const std::filesystem::path& file, bool snapshots) {
  test::ScriptedMarket market;
  auto header = test::recording_header();
  header.started = market.time;
  header.capabilities.delay = 0s;
  header.capabilities.poll_interval = snapshots ? 1s : 0s;
  md::Timestamp receipt = market.time;
  md::RecordingSink::Options options;
  options.clock = [&] { return receipt; };
  test::DiscardEvents discard;
  md::RecordingSink sink(file, header, discard, options);
  sink.publish(md::ContractDefinition{0, market.contract});
  for (int second = 0; second <= 12; ++second) {
    receipt = market.time + second * md::kNanosPerSecond;
    sink.publish(md::UnderlyingQuote{"SPX", receipt, 5000, 5000, 5000});
    sink.publish(md::OptionQuote{0, receipt, 100 + .1 * second, 100.2 + .1 * second, 20, 20});
    if (snapshots) sink.publish(md::SnapshotComplete{"SPX", receipt});
  }
  sink.close();
}
TEST(ReplayBatches, SnapshotAndStreamingBoundariesPreserveEveryQuote) {
  for (const bool snapshots : {false, true}) {
    test::RecordingFile file;
    write_stream(file.path, snapshots);
    md::RecordingReader reader(file.path);
    providers::ReplayBatches batches(reader, reader.header().subscription);
    std::size_t quotes = 0, count = 0;
    md::Timestamp previous = 0;
    while (const auto batch = batches.next()) {
      EXPECT_GT(batch->time, previous);
      previous = batch->time;
      for (const auto& event : batch->events) if (std::holds_alternative<md::OptionQuote>(event)) ++quotes;
      if (snapshots) { EXPECT_TRUE(std::holds_alternative<md::SnapshotComplete>(batch->events.back())); }
      ++count;
    }
    EXPECT_EQ(quotes, 13U);
    EXPECT_EQ(count, 13U);
  }
}

TEST(ReplayBatches, SubsecondStreamQuotesSettleOnTheFixedMarketSecond) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  auto header = test::recording_header();
  header.started = market.time;
  header.capabilities.poll_interval = 0s;
  header.capabilities.delay = 0s;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return market.time; };
  test::DiscardEvents discard;
  {
    md::RecordingSink sink(file.path, header, discard, recording);
    sink.publish(md::ContractDefinition{0, market.contract});
    sink.publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    market.time += md::kNanosPerSecond / 2;
    sink.publish(md::OptionQuote{0, market.time, 100, 100.2, 20, 20});
    sink.close();
  }
  server::Desk::Options options;
  options.replay = true;
  server::Desk desk("test", header.capabilities, header.subscription, options);
  desk.start_trading();
  md::RecordingReader reader(file.path);
  providers::ReplayBatches batches(reader, header.subscription);
  while (const auto batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  ASSERT_TRUE(desk.metrics("SPX"));
  EXPECT_EQ(desk.metrics("SPX")->as_of, market.time + md::kNanosPerSecond / 2);
  EXPECT_EQ(desk.market_time(), desk.metrics("SPX")->as_of);
  providers::ReplayProvider replay({file.path, 0, false, {}, 0, true});
  replay.set_driver([](providers::ReplayBatch) {
    std::promise<void> done;
    done.set_value();
    return done.get_future();
  });
  replay.start({header.subscription.underlyings, 0, 0}, discard);
  ASSERT_TRUE(test::recording_eventually([&] { return !replay.fast_forwarding(); }));
  EXPECT_THROW(replay.until(market.time), std::invalid_argument);
  EXPECT_NO_THROW(replay.until(market.time + md::kNanosPerSecond / 2));
  EXPECT_EQ(replay.settled_through(), market.time + md::kNanosPerSecond / 2);
  replay.stop();
}

/// A snapshot feed that polls QQQ, SPX and SPY in turn at each 15-second market time,
/// in the order the demo market writes them: every underlying's price, then each
/// underlying's one option quote and SnapshotComplete. SPX's option is 0.20 wide, the
/// ETFs' 0.02.
struct PolledInstants {
  test::RecordingFile file;
  std::array<md::OptionContract, 3> contracts{*md::parse_osi("QQQ261022C00450000"),
      *md::parse_osi("SPXW261022C05000000"), *md::parse_osi("SPY261022P00500000")};
  md::Subscription subscription{{"QQQ", "SPX", "SPY"}, 0, 0};
  md::Timestamp open = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  md::Timestamp at(std::size_t instant) const {
    return open + static_cast<md::Timestamp>(instant) * 15 * md::kNanosPerSecond;
  }
  std::string symbol(std::size_t underlying) const { return contracts[underlying].osi_symbol(); }
  /// Each row is one market time's QQQ, SPX and SPY asks.
  void write(const std::vector<std::array<double, 3>>& asks) const {
    auto header = test::recording_header();
    header.started = open;
    header.subscription = subscription;
    header.capabilities.delay = 0s;
    md::Timestamp receipt = open;
    md::RecordingSink::Options options;
    options.clock = [&] { return receipt; };
    test::DiscardEvents discard;
    md::RecordingSink sink(file.path, header, discard, options);
    for (std::size_t u = 0; u < 3; ++u) sink.publish(md::ContractDefinition{static_cast<md::InstrumentId>(u), contracts[u]});
    constexpr std::array<double, 3> spot{450, 5000, 500};
    for (std::size_t i = 0; i < asks.size(); ++i) {
      receipt = at(i);
      for (std::size_t u = 0; u < 3; ++u) sink.publish(md::UnderlyingQuote{subscription.underlyings[u], at(i), spot[u], spot[u], spot[u]});
      for (std::size_t u = 0; u < 3; ++u) {
        receipt = at(i) + static_cast<md::Timestamp>(u) * md::kNanosPerSecond / 1000;
        const double spread = u == 1 ? .2 : .02;
        sink.publish(md::OptionQuote{static_cast<md::InstrumentId>(u), at(i), asks[i][u] - spread, asks[i][u], 20, 20});
        sink.publish(md::SnapshotComplete{subscription.underlyings[u], at(i)});
      }
    }
    sink.close();
  }
  server::Desk desk(server::Desk::Options options) const {
    options.analytics.fallback_rate = 0;  // one call per chain has no put to imply a rate from
    md::RecordingReader reader(file.path);
    return server::Desk("replay (synthetic)", reader.header().capabilities, subscription, std::move(options));
  }
};
server::TradingReply order(server::Desk& desk, const std::string& symbol, trading::Side side, trading::Quantity quantity,
                           md::Timestamp time) {
  server::TradingCommand request;
  request.order = {symbol + (side == trading::Side::Buy ? ":buy" : ":sell"), symbol, side, trading::OrderType::Market,
                   trading::TimeInForce::Ioc, quantity, {}, {}, {}, {}};
  return command(desk, request, time, time);
}

TEST(ReplayBatches, EveryUnderlyingsSnapshotOfOneMarketTimeIsOneBatch) {
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 2.12}, {4.22, 12.20, 2.22}});
  for (const bool instants : {true, false}) {
    md::RecordingReader reader(feed.file.path);
    providers::ReplayBatches batches(reader, feed.subscription, instants);
    std::vector<providers::ReplayBatch> all;
    while (auto batch = batches.next()) all.push_back(std::move(*batch));
    // Runs recorded before driver 2 batched each underlying's snapshot alone.
    ASSERT_EQ(all.size(), instants ? 3U : 9U);
    for (std::size_t i = 0; i < all.size(); ++i) {
      const auto instant = instants ? i : i / 3;
      EXPECT_EQ(all[i].time, feed.at(instant));
      const auto completes = std::count_if(all[i].events.begin(), all[i].events.end(),
          [](const md::Event& event) { return std::holds_alternative<md::SnapshotComplete>(event); });
      EXPECT_EQ(completes, instants ? 3 : 1);
      EXPECT_TRUE(std::holds_alternative<md::SnapshotComplete>(all[i].events.back()));
    }
    // The batch settles at the last snapshot's receipt, as it did before.
    EXPECT_EQ(all.back().received, feed.at(2) + 2 * md::kNanosPerSecond / 1000);
  }
  // A snapshot that changed nothing moves no clock: its SnapshotComplete's time places it,
  // with the others of its time and never with an earlier time's.
  auto header = test::recording_header();
  header.subscription = feed.subscription;
  const auto unchanged = feed.file.directory / "unchanged.oprec";
  test::record_events(unchanged, {md::ContractDefinition{0, feed.contracts[1]}, md::ContractDefinition{1, feed.contracts[2]},
      md::UnderlyingQuote{"SPX", feed.at(0), 5000, 5000, 5000}, md::OptionQuote{0, feed.at(0), 10, 10.2, 20, 20},
      md::SnapshotComplete{"SPX", feed.at(0)}, md::SnapshotComplete{"SPY", feed.at(0)},
      md::SnapshotComplete{"SPX", feed.at(1)}, md::OptionQuote{1, feed.at(1), 2, 2.02, 20, 20},
      md::SnapshotComplete{"SPY", feed.at(1)}, md::SnapshotComplete{"SPX", feed.at(2)}}, header);
  md::RecordingReader reader(unchanged);
  providers::ReplayBatches batches(reader, feed.subscription);
  std::vector<std::pair<std::size_t, md::Timestamp>> shapes;
  while (const auto batch = batches.next()) shapes.emplace_back(batch->events.size(), batch->time);
  EXPECT_EQ(shapes, (std::vector<std::pair<std::size_t, md::Timestamp>>{{6, feed.at(0)}, {3, feed.at(1)}, {1, feed.at(1)}}));
}

TEST(ReplayBatches, ATruncatedRecordingDeliversItsLastCompleteInstantBeforeFailing) {
  // A recorder killed before its clean end leaves every complete record readable. Looking
  // ahead for the rest of an instant meets the truncation, but the last whole instant is
  // still delivered, and only the next call fails, as without instants.
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 2.12}});
  std::filesystem::resize_file(feed.file.path, std::filesystem::file_size(feed.file.path) - 1);
  for (const bool instants : {true, false}) {
    md::RecordingReader reader(feed.file.path);
    providers::ReplayBatches batches(reader, feed.subscription, instants);
    std::vector<md::Timestamp> times;
    const auto drain = [&] { while (const auto batch = batches.next()) times.push_back(batch->time); };
    EXPECT_THROW(drain(), std::runtime_error);
    ASSERT_EQ(times.size(), instants ? 2U : 6U);
    EXPECT_EQ(times.back(), feed.at(1));
    EXPECT_THROW((void)batches.next(), std::runtime_error);
  }
}

TEST(MarketInstants, LatencyFillsOnTheNextInstantsQuoteNotOneAnotherUnderlyingReoffered) {
  // B01: QQQ's snapshot of 10:00:15 comes first. Applied alone, it re-offered SPX's
  // 10:00 quote at 10:00:15, which released the delayed order at the price it was sent at.
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 2.12}});
  server::Desk::Options options;
  options.replay = true;
  options.paper.rules.fill_latency_ms = 1000;
  auto desk = feed.desk(options);
  desk.start_trading();
  md::RecordingReader reader(feed.file.path);
  providers::ReplayBatches batches(reader, feed.subscription);
  auto batch = batches.next();
  for (; batch && batch->time == feed.at(0); batch = batches.next())
    desk.replay_batch(batch->events, batch->received, batch->time);
  const auto reply = order(desk, feed.symbol(1), trading::Side::Buy, 1, feed.at(0));
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  EXPECT_TRUE(reply.view->snapshot->recent_fills.empty());
  for (; batch; batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  const auto& fills = desk.trading_view()->snapshot->recent_fills;
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills.front().price, Money::parse("11.20"));
  EXPECT_EQ(fills.front().quote_time, feed.at(1));
}

TEST(MarketInstants, AnotherUnderlyingsLivePollKeepsAQuoteCurrentButDoesNotMakeItNew) {
  // A live polling feed delivers each underlying's snapshot on its own, as ReplayBatches
  // without instants cuts the recording. QQQ's newer poll moves the market clock, but
  // SPX's quote was supplied before the order, so the order's latency waits for SPX's.
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 2.12}});
  server::Desk::Options options;
  options.paper.rules.fill_latency_ms = 1000;
  auto desk = feed.desk(options);
  desk.start_trading();
  md::RecordingReader reader(feed.file.path);
  providers::ReplayBatches polls(reader, feed.subscription, false);
  const auto poll = [&] {
    const auto batch = polls.next();
    ASSERT_TRUE(batch);
    desk.replay_batch(batch->events, batch->received, batch->time);
  };
  for (int underlying = 0; underlying < 3; ++underlying) poll();
  const auto reply = order(desk, feed.symbol(1), trading::Side::Buy, 1, feed.at(0));
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  poll();  // QQQ at 10:00:15
  EXPECT_EQ(desk.market_time(), feed.at(1));
  EXPECT_TRUE(desk.trading_view()->snapshot->recent_fills.empty());
  EXPECT_TRUE(desk.trading_view()->snapshot->valuation_complete);
  poll();  // SPX at 10:00:15
  const auto& fills = desk.trading_view()->snapshot->recent_fills;
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills.front().price, Money::parse("11.20"));
  EXPECT_EQ(fills.front().quote_time, feed.at(1));
}

TEST(MarketInstants, TheExpiryCutoffClosesOnTheCutoffInstantsBook) {
  // B02: the 15:55:00 auto-close ran on QQQ's 15:55:00 snapshot and sold into SPX's
  // 15:54:45 bid.
  PolledInstants feed;
  feed.contracts[1] = *md::parse_osi("SPXW260922C05000000");
  feed.open = md::new_york_to_utc({2026, 9, 22}, 15, 54) + 45 * md::kNanosPerSecond;
  feed.write({{4.02, 3.20, 2.02}, {4.12, 2.60, 2.12}});
  server::Desk::Options options;
  options.replay = true;
  options.paper.rules.expiry_cutoff = 300 * md::kNanosPerSecond;
  auto desk = feed.desk(options);
  desk.start_trading();
  md::RecordingReader reader(feed.file.path);
  providers::ReplayBatches batches(reader, feed.subscription);
  auto batch = batches.next();
  for (; batch && batch->time == feed.at(0); batch = batches.next())
    desk.replay_batch(batch->events, batch->received, batch->time);
  ASSERT_TRUE(order(desk, feed.symbol(1), trading::Side::Buy, 2, feed.at(0)).decision.ok());
  for (; batch; batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  const auto view = desk.trading_view();
  EXPECT_TRUE(view->snapshot->positions.empty());
  const auto& fills = view->snapshot->recent_fills;
  ASSERT_EQ(fills.size(), 2U);
  EXPECT_EQ(fills.back().side, trading::Side::Sell);
  EXPECT_EQ(fills.back().price, Money::parse("2.40"));
  EXPECT_EQ(fills.back().time, feed.at(1));
  EXPECT_EQ(fills.back().quote_time, feed.at(1));
}

TEST(MarketInstants, TrailingDrawdownIsDecidedOnWholeInstants) {
  // B03: long an SPX call and SPY puts. At 10:00:15 SPX gains 100.00 and the puts lose
  // 120.00: equity falls. SPX's snapshot applied before SPY's showed a gain that never
  // existed, and the intraday peak and floor ratcheted on it.
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 1.90}});
  server::Desk::Options options;
  options.replay = true;
  options.paper.initial_cash = Money::parse("25000");
  options.paper.rules.max_drawdown = Money::parse("1250");
  options.paper.rules.drawdown_mode = trading::DrawdownMode::Intraday;
  auto desk = feed.desk(options);
  desk.start_trading();
  md::RecordingReader reader(feed.file.path);
  providers::ReplayBatches batches(reader, feed.subscription);
  auto batch = batches.next();
  for (; batch && batch->time == feed.at(0); batch = batches.next())
    desk.replay_batch(batch->events, batch->received, batch->time);
  ASSERT_TRUE(order(desk, feed.symbol(1), trading::Side::Buy, 1, feed.at(0)).decision.ok());
  ASSERT_TRUE(order(desk, feed.symbol(2), trading::Side::Buy, 10, feed.at(0)).decision.ok());
  const auto before = desk.trading_view()->snapshot->equity;
  for (; batch; batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  const auto snapshot = desk.trading_view()->snapshot;
  EXPECT_EQ(snapshot->equity, before - Money::parse("20"));
  EXPECT_EQ(snapshot->evaluation.peak, Money::parse("25000"));
  EXPECT_EQ(snapshot->evaluation.floor, Money::parse("23750"));
}

TEST(MarketInstants, AMinutesFirstEquitySampleHoldsThatInstantsEquity) {
  // B21: QQQ's 10:01:00 snapshot sampled the account before SPX's arrived, and the
  // minute's first sample is the one the equity history keeps.
  PolledInstants feed;
  feed.open = md::new_york_to_utc({2026, 9, 22}, 10, 0) + 45 * md::kNanosPerSecond;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 12.20, 2.12}});
  std::vector<server::EquitySample> samples;
  server::Desk::Options options;
  options.replay = true;
  options.equity_sample = [&](std::string_view, const server::EquitySample& sample) { samples.push_back(sample); };
  auto desk = feed.desk(options);
  desk.start_trading();
  md::RecordingReader reader(feed.file.path);
  providers::ReplayBatches batches(reader, feed.subscription);
  auto batch = batches.next();
  for (; batch && batch->time == feed.at(0); batch = batches.next())
    desk.replay_batch(batch->events, batch->received, batch->time);
  ASSERT_TRUE(order(desk, feed.symbol(1), trading::Side::Buy, 1, feed.at(0)).decision.ok());
  for (; batch; batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  const auto equity = desk.trading_view()->snapshot->equity;
  std::size_t minute = 0;
  for (const auto& sample : samples) {
    if (sample.time != feed.at(1) || sample.fill || sample.stock_fill) continue;
    EXPECT_EQ(sample.equity, equity);
    ++minute;
  }
  EXPECT_GT(minute, 0U);
}

TEST(ReproducibleRun, RunsRecordedBeforeInstantBatchesStillVerify) {
  // Driver 2 changes fills, so a run records it; one without it replays with the
  // per-snapshot batches and re-offered quotes it was recorded with.
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 2.12}});
  for (const bool instants : {false, true}) {
    const auto journal = feed.file.directory / (instants ? "driver-2.jsonl" : "driver-1.jsonl");
    {
      server::Desk::Options options;
      options.replay = true;
      options.instant_batches = instants;
      options.run_input = server::recording_input(feed.file.path);
      options.paper_journal = journal;
      options.paper.rules.fill_latency_ms = 1000;
      auto desk = feed.desk(options);
      desk.start_trading();
      md::RecordingReader reader(feed.file.path);
      providers::ReplayBatches batches(reader, feed.subscription, instants);
      auto batch = batches.next();
      for (; batch && batch->time == feed.at(0); batch = batches.next())
        desk.replay_batch(batch->events, batch->received, batch->time);
      ASSERT_TRUE(order(desk, feed.symbol(1), trading::Side::Buy, 1, feed.at(0)).decision.ok());
      for (; batch; batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
      const auto& fills = desk.trading_view()->snapshot->recent_fills;
      ASSERT_EQ(fills.size(), 1U);
      EXPECT_EQ(fills.front().price, Money::parse(instants ? "11.20" : "10.20"));
      desk.stop();
    }
    // Driver 3 batches instants too.
    EXPECT_EQ(read_file(journal).find("\"driver\":3") != std::string::npos, instants);
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}

/// SPY's close on 2026-09-22 and its open on 2026-09-23, 4% lower: a call and a deep
/// put a month out. At the close the put keeps 0.50 of time value; at the open it
/// trades 0.20 under its exercise value.
struct Overnight {
  test::RecordingFile file;
  md::OptionContract call = *md::parse_osi("SPY261016C00500000");
  md::OptionContract put = *md::parse_osi("SPY261016P00540000");
  md::Subscription subscription{{"SPY"}, 0, 0};
  struct Instant {
    md::Timestamp time;
    double spot, call_ask, put_ask;
  };
  std::vector<Instant> instants{{md::new_york_to_utc({2026, 9, 22}, 15, 59, 45), 500, 10.20, 40.60},
                                {md::new_york_to_utc({2026, 9, 22}, 16, 14, 45), 500, 10.20, 40.60},
                                {md::new_york_to_utc({2026, 9, 23}, 9, 30), 480, 3.20, 59.90},
                                {md::new_york_to_utc({2026, 9, 23}, 9, 30, 15), 480, 3.20, 59.90}};
  Overnight() {
    auto header = test::recording_header();
    header.started = instants.front().time;
    header.subscription = subscription;
    header.capabilities.delay = 0s;
    md::Timestamp receipt = header.started;
    md::RecordingSink::Options options;
    options.clock = [&] { return receipt; };
    test::DiscardEvents discard;
    md::RecordingSink sink(file.path, header, discard, options);
    sink.publish(md::ContractDefinition{0, call});
    sink.publish(md::ContractDefinition{1, put});
    for (const auto& instant : instants) {
      receipt = instant.time;
      sink.publish(md::UnderlyingQuote{"SPY", instant.time, instant.spot, instant.spot + .01, instant.spot});
      sink.publish(md::OptionQuote{0, instant.time, instant.call_ask - .2, instant.call_ask, 20, 20});
      sink.publish(md::OptionQuote{1, instant.time, instant.put_ask - .2, instant.put_ask, 20, 20});
      sink.publish(md::SnapshotComplete{"SPY", instant.time});
    }
    sink.close();
  }
  /// Long a call and short ten puts from the first instant on, to the end.
  std::shared_ptr<const trading::TradingSnapshot> run(server::Desk::Options options) const {
    options.replay = true;
    options.analytics.fallback_rate = 0;
    options.paper.limits.aggregate = {1e12, 1e12};
    options.paper.limits.per_underlying = {1e12, 1e12};
    options.paper.limits.max_daily_loss = Money::parse("1000000");
    md::RecordingReader reader(file.path);
    server::Desk desk("replay (synthetic)", reader.header().capabilities, subscription, std::move(options));
    desk.start_trading();
    providers::ReplayBatches batches(reader, subscription);
    auto batch = batches.next();
    desk.replay_batch(batch->events, batch->received, batch->time);
    EXPECT_TRUE(order(desk, call.osi_symbol(), trading::Side::Buy, 1, instants.front().time).decision.ok());
    EXPECT_TRUE(order(desk, put.osi_symbol(), trading::Side::Sell, 10, instants.front().time).decision.ok());
    while ((batch = batches.next())) desk.replay_batch(batch->events, batch->received, batch->time);
    auto snapshot = desk.trading_view()->snapshot;
    desk.stop();
    return snapshot;
  }
};

TEST(Rollover, TheFinishedDayClosesOnItsClosingMarksBeforeTheNextDaysQuotes) {
  // B28 and D11: the first batch of 2026-09-23 was applied before the rollover, so the
  // finished day's P&L by Greek took the overnight gap and the puts were assigned on
  // the opening marks. Runs recorded before driver 3 still roll over that way.
  Overnight feed;
  for (const bool closing : {true, false}) {
    SCOPED_TRACE(closing ? "driver 3" : "driver 2");
    const auto journal = feed.file.directory / (closing ? "driver-3.jsonl" : "driver-2.jsonl");
    server::Desk::Options options;
    options.closing_rollover = closing;
    options.run_input = server::recording_input(feed.file.path);
    options.paper_journal = journal;
    const auto snapshot = feed.run(options);
    const auto& e = snapshot->evaluation;
    ASSERT_EQ(e.days.size(), 1U);
    const auto& finished = e.days.front();
    EXPECT_EQ(finished.day, (md::Date{2026, 9, 22}));
    const double finished_pnl = (finished.close_equity - finished.open_equity).dollars();
    const double today_pnl = (snapshot->equity - e.day_open_equity).dollars();
    if (closing) {
      // The finished day's parts add up to its close less its open; the gap is the new day's.
      EXPECT_NEAR(finished.attribution.total(), finished_pnl, 1e-6);
      EXPECT_EQ(e.day_open_equity, finished.close_equity);
      EXPECT_NEAR(snapshot->attribution.total(), today_pnl, 1e-6);
      EXPECT_LT(today_pnl, -10000);
      // At the close the puts were worth more than their exercise: nobody exercises them.
      EXPECT_TRUE(snapshot->closures.empty());
      EXPECT_TRUE(snapshot->stock_fills.empty());
    } else {
      EXPECT_LT(finished.attribution.total(), finished_pnl - 10000);
      EXPECT_LT(e.day_open_equity, finished.close_equity - Money::parse("10000"));
      EXPECT_FALSE(snapshot->closures.empty());
    }
    const auto text = read_file(journal);
    EXPECT_NE(text.find(closing ? "\"driver\":3" : "\"driver\":2"), std::string::npos);
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}

class ImmediateClock final : public providers::ReplayClock {
 public:
  TimePoint now() override { return now_; }
  bool wait_until(TimePoint deadline, const std::atomic<bool>& stop) override { now_ = deadline; return !stop; }
  void interrupt() override {}
 private:
  TimePoint now_{};
};

void scripted_orders(server::Desk& desk, md::Timestamp open, md::Timestamp receipt) {
  const auto time = desk.market_time();
  server::TradingCommand request;
  if (time == open + md::kNanosPerMinute) {
    request.order.client_order_id = "vertical";
    request.order.quantity = 1;
    request.order.type = trading::OrderType::Market;
    request.order.tif = trading::TimeInForce::Ioc;
    request.order.legs = {{md::parse_osi("SPXW261127C06000000")->osi_symbol(), trading::Side::Buy, 1}, {md::parse_osi("SPXW261127C06010000")->osi_symbol(), trading::Side::Sell, 1}};
  } else if (time == open + 2 * md::kNanosPerMinute) {
    request.order.client_order_id = "bracket";
    request.order.symbol = md::parse_osi("SPXW261127P06000000")->osi_symbol();
    request.order.quantity = 1;
    request.order.type = trading::OrderType::Market;
    request.order.tif = trading::TimeInForce::Ioc;
    request.order.bracket = trading::Bracket{
        trading::ExitSpec{trading::Trigger{trading::TriggerSource::Underlying, trading::TriggerDirection::AtOrBelow, Money::parse("5800")}, {}},
        trading::ExitSpec{{}, Money::parse("200")}};
  } else if (time == open + 10 * md::kNanosPerMinute) request.kind = server::TradingCommand::Kind::ClosePositions;
  else return;
  const auto reply = command(desk, request, time, receipt);
  EXPECT_TRUE(reply.error_code.empty()) << reply.error_code;
  EXPECT_TRUE(reply.decision.ok()) << reply.decision.message;
}

TEST(ReproducibleRun, ScenarioDayHasIdenticalBytesAcrossSpeedsFastForwardAndVerification) {
  test::RecordingFile file;
  const auto source = file.directory / "golden.json";
  {
    std::ofstream out(source);
    out << R"({"id":"golden","title":"Golden day","description":"Simulated test day","symbols":["SPX"],"session":"regular","date":"2026-11-27","seed":81723,"generator":1,"drift":[[1,0.001]],"volatility":0.12,"iv_shift":0,"spot_vol":-2})";
  }
  const auto scenario = providers::read_scenario(source);
  providers::write_scenario_recording(file.path, scenario, scenario.date, scenario.seed);
  const auto open = providers::scenario_open(scenario, scenario.date);
  std::string golden;
  std::vector<trading::Fill> fills;
  Money equity;
  for (const int speed : {-1, 1, 300, 0}) {
    const auto journal = file.directory / ("run-" + std::to_string(speed) + ".jsonl");
    md::RecordingReader reader(file.path);
    server::Desk::Options options;
    options.replay = true;
    options.run_input = server::scenario_input(scenario, scenario.date, scenario.seed);
    options.paper_journal = journal;
    server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    if (speed == -1) {
      providers::ReplayBatches batches(reader, reader.header().subscription);
      while (const auto batch = batches.next()) {
        desk.replay_batch(batch->events, batch->received, batch->time);
        scripted_orders(desk, open, batch->received);
      }
    } else {
      providers::ReplayProvider replay({file.path, speed, false, std::make_shared<ImmediateClock>()});
      replay.set_driver([&](providers::ReplayBatch batch) {
        std::promise<void> done;
        desk.replay_batch(batch.events, batch.received, batch.time);
        scripted_orders(desk, open, batch.received);
        done.set_value();
        return done.get_future();
      });
      test::DiscardEvents discard;
      replay.start(reader.header().subscription, discard);
      const auto deadline = std::chrono::steady_clock::now() + 60s;
      while (!replay.finished() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
      ASSERT_TRUE(replay.finished());
      replay.stop();
    }
    ASSERT_TRUE(desk.trading_status().enabled) << desk.trading_status().reason;
    const auto view = desk.trading_view();
    ASSERT_TRUE(view);
    EXPECT_TRUE(view->snapshot->positions.empty());
    EXPECT_TRUE(view->snapshot->open_orders.empty());
    ASSERT_GE(view->snapshot->recent_fills.size(), 6U);
    if (golden.empty()) {
      golden = read_file(journal);
      fills = view->snapshot->recent_fills.to_vector();
      equity = view->snapshot->equity;
      json summary = json::array();
      for (const auto& fill : fills) summary.push_back({fill.symbol, fill.quantity, fill.price.micros(), fill.fee.micros(), fill.time});
      // Pin money, quantities and fill times independently of analytic floats.
      EXPECT_EQ(summary, json::parse(R"([
        ["SPXW  261127C06000000",1,11700000,650000,1795789860000000000],
        ["SPXW  261127C06010000",1,5200000,650000,1795789860000000000],
        ["SPXW  261127P06000000",1,4100000,650000,1795789920000000000],
        ["SPXW  261127C06010000",1,4800000,650000,1795790400000000000],
        ["SPXW  261127C06000000",1,10500000,650000,1795790400000000000],
        ["SPXW  261127P06000000",1,4200000,650000,1795790400000000000]
      ])"));
      EXPECT_EQ(equity.micros(), 99926100000LL);
    } else {
      EXPECT_TRUE(read_file(journal) == golden);
      EXPECT_EQ(json(view->snapshot->recent_fills), json(fills));
      EXPECT_EQ(view->snapshot->equity, equity);
    }
    desk.stop();
  }
  const auto journal = file.directory / "run-0.jsonl";
  const auto calendar = md::scheduled_days();
  md::set_scheduled_days({{scenario.date, "Different host calendar", true, 13, 0}});
  const auto verified = server::verify_run(journal);
  EXPECT_EQ(md::scheduled_days().front().name, "Different host calendar");
  md::set_scheduled_days(calendar);
  EXPECT_TRUE(verified.matched) << verified.message;
  EXPECT_EQ(verified.equity, equity);
  const auto altered = file.directory / "altered.jsonl";
  std::uint64_t changed_at = 0;
  {
    auto output = trading::FileJournal::create(altered.string());
    for (const auto& record : trading::FileJournal::read(journal.string()).records) {
      auto payload = json::parse(record.payload);
      if (record.type == "market" && changed_at == 0) { payload["unexpected"] = true; changed_at = record.seq; }
      output->append(record.time, record.type, payload.dump());
    }
  }
  ASSERT_NE(changed_at, 0U);
  const auto mismatch = server::verify_run(altered);
  EXPECT_FALSE(mismatch.matched);
  EXPECT_NE(mismatch.message.find("First differing transaction " + std::to_string(changed_at)), std::string::npos) << mismatch.message;
  // The CLI takes the same path without starting a feed, HTTP or user directories.
  const auto cli = std::string(OPENPORT_APPS_DIR) + "/openportd --verify-run " + journal.string() + " > " + (file.directory / "verify.txt").string() + " 2>&1";
  EXPECT_EQ(std::system(cli.c_str()), 0);
  auto tampered = golden;
  const auto hash = tampered.find("\"hash\":\"");
  ASSERT_NE(hash, std::string::npos);
  tampered[hash + 8] = tampered[hash + 8] == 'a' ? 'b' : 'a';
  { std::ofstream out(journal); out << tampered; }
  EXPECT_FALSE(server::verify_run(journal).matched);
  EXPECT_NE(std::system(cli.c_str()), 0);
  { std::ofstream out(journal); out << golden; }
  { std::ofstream out(source, std::ios::app); out << '\n'; }
  const auto changed = server::verify_run(journal);
  EXPECT_FALSE(changed.matched);
  EXPECT_NE(changed.message.find("Scenario input changed"), std::string::npos) << changed.message;
}

server::ApiResponse replay_call(server::ReplayHost& host, std::string method, std::string target, json body = {}) {
  std::promise<server::ApiResponse> done;
  auto result = done.get_future();
  server::ApiRequest request{std::move(method), std::move(target), body.dump()};
  request.content_type = "application/json";
  request.actor = "test-actor";
  if (!host.handle(request, [&](server::ApiResponse response) { done.set_value(std::move(response)); }))
    throw std::runtime_error("Replay request was not handled");
  if (result.wait_for(5min) != std::future_status::ready) throw std::runtime_error("Replay request timed out");
  return result.get();
}
TEST(ReplayRun, KeptJournalFlushesAtPauseStepFinishStopAndTeardown) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const test::ScriptedMarket market;
  for (const std::string ending : {"finish", "stop", "teardown"}) {
    const auto directory = file.directory / ending;
    std::filesystem::create_directory(directory);
    std::atomic<std::uintmax_t> synced_bytes{0};
    std::atomic<unsigned> syncs{0};
    server::Engine::Options options;
    options.paper_journal = directory / "main.jsonl";
    options.journal_io.clock = [] { return std::chrono::steady_clock::time_point{}; };
    options.journal_io.sync = [&](int fd) {
      synced_bytes = static_cast<std::uintmax_t>(::lseek(fd, 0, SEEK_END));
      ++syncs;
      return true;
    };
    std::filesystem::path journal;
    {
      server::ReplayHost host({file.directory, options, false});
      const auto started = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"speed", 0}, {"paused", true}});
      ASSERT_EQ(started.status, 201) << started.body;
      const auto id = json::parse(started.body).at("replay").at("id").get<std::string>();
      journal = directory / "replays" / (id + ".jsonl");
      ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
      EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(journal));
      const auto initial_syncs = syncs.load();
      const auto stepped = replay_call(host, "PUT", "/api/replay", {{"until", md::format_timestamp(market.time + 4 * md::kNanosPerSecond)}});
      ASSERT_EQ(stepped.status, 200) << stepped.body;
      EXPECT_GT(syncs.load(), initial_syncs);
      EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(journal));
      // Pausing explicitly also waits for the owner-thread durability barrier.
      ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"paused", true}}).status, 200);
      EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(journal));
      if (ending == "finish") {
        ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"paused", false}}).status, 200);
        ASSERT_TRUE(test::recording_eventually([&] { return json::parse(host.tick()).at("replay").at("finished").get<bool>(); }));
        EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(journal));
        const auto listing = json::parse(replay_call(host, "GET", "/api/replay").body);
        EXPECT_EQ(listing.at("history").size(), 1U);
        EXPECT_EQ(replay_call(host, "GET", "/api/replay/history/" + id).status, 200);
      } else {
        ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"speed", 1}, {"paused", false}}).status, 200);
        if (ending == "stop") {
          EXPECT_EQ(replay_call(host, "DELETE", "/api/replay").status, 200);
          EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(journal));
        }
      }
    }
    EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(journal));
    const auto recovered = trading::FileJournal::read(journal.string());
    EXPECT_LT(syncs.load(), recovered.records.size());
    EXPECT_TRUE(server::verify_run(journal).matched);
  }
}

TEST(ReplayRun, HistoryWaitsForTheStoppingJournalsFinalSync) {
  test::RecordingFile file;
  write_stream(file.path, true);
  std::promise<void> syncing, release;
  auto entered = syncing.get_future();
  const auto released = release.get_future().share();
  std::atomic<bool> hold{false};
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  options.analytics.fallback_rate = 0;
  options.journal_io.clock = [] { return std::chrono::steady_clock::time_point{}; };
  options.journal_io.sync = [&](int) {
    if (hold.exchange(false)) { syncing.set_value(); released.wait(); }
    return true;
  };
  server::ReplayHost host({file.directory, options, false});
  const auto started = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"speed", 1}, {"paused", true}});
  ASSERT_EQ(started.status, 201) << started.body;
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
  const auto paused_time = json::parse(host.tick()).at("replay").at("settled_through");
  ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"paused", false}}).status, 200);
  // A pending pause barrier can still flush the next command. A resumed batch
  // proves that barrier has completed before we leave an unsynced record.
  ASSERT_TRUE(test::recording_eventually([&] {
    return json::parse(host.tick()).at("replay").at("settled_through") != paused_time;
  }));
  // This command records input even if the order is rejected. Leave it unsynced.
  const test::ScriptedMarket market;
  const auto ordered = replay_call(host, "POST", "/api/replay/orders", {{"client_order_id", "before-stop"}, {"symbol", market.symbol()},
      {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}});
  ASSERT_EQ(ordered.status, 201) << ordered.body;
  hold = true;
  auto stopping = std::async(std::launch::async, [&] { host.stop(); });
  // Wait for the sync itself; the deadline only guards against a stuck worker.
  const auto waiting = entered.wait_for(5min);
  EXPECT_EQ(waiting, std::future_status::ready);
  if (waiting != std::future_status::ready) { release.set_value(); stopping.get(); return; }
  auto listing = std::async(std::launch::async, [&] { return replay_call(host, "GET", "/api/replay"); });
  EXPECT_EQ(listing.wait_for(20ms), std::future_status::timeout);
  release.set_value();
  stopping.get();
  const auto response = listing.get();
  ASSERT_EQ(response.status, 200) << response.body;
  EXPECT_EQ(json::parse(response.body).at("history").size(), 1U);
}

TEST(ReplayRun, StandaloneEngineFlushesBeforePublishingFinished) {
  test::RecordingFile file;
  write_stream(file.path, true);
  std::atomic<std::uintmax_t> synced_bytes{0};
  std::atomic<unsigned> syncs{0};
  providers::ReplayProvider replay({file.path, 0, false, {}});
  server::Engine::Options options;
  options.paper_journal = file.directory / "run.jsonl";
  options.journal_io.clock = [] { return std::chrono::steady_clock::time_point{}; };
  options.journal_io.sync = [&](int fd) {
    synced_bytes = static_cast<std::uintmax_t>(::lseek(fd, 0, SEEK_END));
    ++syncs;
    return true;
  };
  server::Engine engine(replay, {{"SPX"}}, options);
  engine.start();
  ASSERT_TRUE(test::recording_eventually([&] { return replay.finished(); }));
  EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(options.paper_journal));
  EXPECT_EQ(syncs.load(), 2U);  // First record and EOF, regardless of the market clock.
  EXPECT_TRUE(server::verify_run(options.paper_journal).matched);
  engine.stop();
}

TEST(ReproducibleRun, LockstepSmallAndLargeStepsMatchContinuousCommandsAndVerify) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const test::ScriptedMarket market;
  std::string golden;
  // Continuous headless run is the reference, including identical command instants.
  for (const int step : {0, 1, 4}) {
    const auto directory = file.directory / std::to_string(step);
    std::filesystem::create_directory(directory);
    std::filesystem::path journal;
    if (step == 0) {
      journal = directory / "run.jsonl";
      md::RecordingReader reader(file.path);
      server::Desk::Options options;
      options.replay = true;
      options.initial_actor = "test-actor";
      options.run_input = server::recording_input(file.path);
      options.paper_journal = journal;
      // ReplayHost's default plan is practice.
      options.paper.rules = server::find_plan("practice")->rules;
      options.paper.initial_cash = server::find_plan("practice")->initial_cash;
      server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
      desk.start_trading();
      providers::ReplayBatches batches(reader, reader.header().subscription);
      while (const auto batch = batches.next()) {
        desk.replay_batch(batch->events, batch->received, batch->time);
        if (desk.market_time() == market.time + 4 * md::kNanosPerSecond) {
          server::TradingCommand order;
          order.actor = "test-actor";
          order.order = market.market("lockstep");
          EXPECT_TRUE(command(desk, order, desk.market_time(), batch->received).decision.ok());
        }
      }
      desk.stop();
      golden = read_file(journal);
    } else {
      server::Engine::Options options;
      options.paper_journal = directory / "main.jsonl";
      server::ReplayHost host({file.directory, options, false});
      const auto response = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}, {"speed", 0}});
      ASSERT_EQ(response.status, 201) << response.body;
      const auto id = json::parse(response.body).at("replay").at("id").get<std::string>();
      ASSERT_TRUE(test::recording_eventually([&] {
        return !json::parse(replay_call(host, "GET", "/api/replay").body).at("replay").at("fast_forwarding").get<bool>();
      }));
      for (int second = step; second <= 12; second += step) {
        const auto time = market.time + second * md::kNanosPerSecond;
        const auto target = second % 2 == 0 ? md::format_timestamp(time) : "10:00:" + (second < 10 ? std::string("0") : std::string()) + std::to_string(second);
        const auto stepped = replay_call(host, "PUT", "/api/replay", {{"until", target}});
        ASSERT_EQ(stepped.status, 200) << stepped.body;
        EXPECT_EQ(json::parse(stepped.body).at("settled_through"), md::format_timestamp(time));
        if (second == 4) {
          const auto bought = replay_call(host, "POST", "/api/replay/orders", {{"client_order_id", "lockstep"}, {"symbol", market.symbol()},
              {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}});
          ASSERT_EQ(bought.status, 201) << bought.body;
          if (step == 1) {
            const auto gap = replay_call(host, "PUT", "/api/replay", {{"until", md::format_timestamp(time + md::kNanosPerSecond / 2)}});
            ASSERT_EQ(gap.status, 200) << gap.body;
            const auto preview = replay_call(host, "POST", "/api/replay/orders/preview", {{"client_order_id", "preview"},
                {"symbol", market.symbol()}, {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}});
            EXPECT_EQ(preview.status, 200) << preview.body;
          }
        }
      }
      host.stop();
      journal = directory / "replays" / (id + ".jsonl");
      EXPECT_TRUE(read_file(journal) == golden);
    }
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}
TEST(ReproducibleRun, LockstepWaitsForTheConsumerAndRejectsBackwardAndPastEndTimes) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const test::ScriptedMarket market;
  providers::ReplayProvider replay({file.path, 0, false, {}, 0, true});
  auto held = std::make_shared<std::promise<void>>();
  std::atomic<bool> received{false};
  replay.set_driver([&](providers::ReplayBatch batch) {
    if (batch.time == market.time + md::kNanosPerSecond) {
      received = true;
      return held->get_future();
    }
    std::promise<void> done;
    done.set_value();
    return done.get_future();
  });
  test::DiscardEvents discard;
  replay.start({{"SPX"}}, discard);
  ASSERT_TRUE(test::recording_eventually([&] { return !replay.fast_forwarding(); }));
  auto advancing = std::async(std::launch::async, [&] { replay.until(market.time + md::kNanosPerSecond); });
  ASSERT_TRUE(test::recording_eventually([&] { return received.load(); }));
  EXPECT_EQ(advancing.wait_for(20ms), std::future_status::timeout);
  EXPECT_EQ(replay.settled_through(), market.time);
  held->set_value();
  ASSERT_EQ(advancing.wait_for(5min), std::future_status::ready);
  EXPECT_NO_THROW(advancing.get());
  EXPECT_TRUE(replay.paused());
  EXPECT_EQ(replay.settled_through(), market.time + md::kNanosPerSecond);
  EXPECT_THROW(replay.until(market.time), std::invalid_argument);
  EXPECT_THROW(replay.until(market.time + 20 * md::kNanosPerSecond), std::invalid_argument);
  EXPECT_EQ(replay.settled_through(), market.time + 12 * md::kNanosPerSecond);
  replay.stop();
}

TEST(ReproducibleRun, ConservativeFillsHaveIdenticalBytesAndPassCliVerification) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const test::ScriptedMarket market;
  std::string golden;
  for (int repeat = 0; repeat < 2; ++repeat) {
    const auto journal = file.directory / ("conservative-" + std::to_string(repeat) + ".jsonl");
    {
      md::RecordingReader reader(file.path);
      server::Desk::Options options;
      options.run_input = server::recording_input(file.path);
      options.replay = true;
      options.paper_journal = journal;
      options.paper.limits.aggregate = {1e9, 1e9};
      options.paper.limits.per_underlying = {1e9, 1e9};
      options.paper.rules.fill_latency_ms = 1000;
      options.paper.rules.impact_ticks = 1;
      options.paper.rules.slippage_ticks = 1;
      server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
      desk.start_trading();
      providers::ReplayBatches batches(reader, reader.header().subscription);
      bool submitted = false;
      while (const auto batch = batches.next()) {
        desk.replay_batch(batch->events, batch->received, batch->time);
        if (!submitted) {
          server::TradingCommand request;
          request.order = market.market("conservative", 25);
          const auto reply = command(desk, request, batch->time, batch->received);
          ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
          EXPECT_TRUE(desk.trading_view()->snapshot->recent_fills.empty());
          submitted = true;
        }
      }
      const auto& fills = desk.trading_view()->snapshot->recent_fills;
      ASSERT_EQ(fills.size(), 2U);
      EXPECT_EQ(fills.front().time, market.time + md::kNanosPerSecond);
      EXPECT_EQ(fills.front().quantity, 20);
      EXPECT_EQ(fills.back().quantity, 5);
      EXPECT_EQ(fills.front().price, Money::parse("100.40"));
      EXPECT_EQ(fills.back().price, Money::parse("100.50"));
      desk.stop();
    }
    if (repeat == 0) { golden = read_file(journal); } else { EXPECT_EQ(read_file(journal), golden); }
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
    const auto cli = std::string(OPENPORT_APPS_DIR) + "/openportd --verify-run " + journal.string() +
                     " > " + (file.directory / "verify-fills.txt").string() + " 2>&1";
    EXPECT_EQ(std::system(cli.c_str()), 0) << read_file(file.directory / "verify-fills.txt");
  }
}

TEST(ReproducibleRun, MissingRecordingAndOldJournalsHaveClearDiagnostics) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const auto journal = file.directory / "run.jsonl";
  {
    md::RecordingReader reader(file.path);
    server::Desk::Options options;
    options.run_input = server::recording_input(file.path);
    options.replay = true;
    options.paper_journal = journal;
    server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    providers::ReplayBatches batches(reader, reader.header().subscription);
    desk.replay_batch(batches.next()->events, reader.header().started);
    desk.set_dividends({{"SPX", {2026, 9, 23}, Money::parse("1.00")}});
  }
  EXPECT_TRUE(server::verify_run(journal).matched);
  { std::ofstream out(file.path, std::ios::app); out << 'x'; }
  EXPECT_NE(server::verify_run(journal).message.find("Recording input changed"), std::string::npos);
  std::filesystem::remove(file.path);
  EXPECT_NE(server::verify_run(journal).message.find("Missing input"), std::string::npos);
  const auto old = file.directory / "old.jsonl";
  { trading::TradingSession session({}, 0, trading::FileJournal::create(old.string())); }
  EXPECT_NO_THROW((void)trading::TradingSession::recover(trading::FileJournal::read(old.string())));
  EXPECT_NE(server::verify_run(old).message.find("older journals still load"), std::string::npos);
}
}  // namespace

namespace {
TEST(ReplayRun, ContractFixture) {
  test::RecordingFile file;
  write_stream(file.path, true);
  server::Engine::Options options;
  options.paper_journal = file.directory / "paper.jsonl";
  options.write_mode = "open";
  server::ReplayHost host({file.directory, options, false});
  const auto capture = [&](std::string method, std::string path, json body = json::object()) {
    const auto response = replay_call(host, method, path, body);
    EXPECT_GE(response.status, 200);
    EXPECT_LT(response.status, 300) << response.body;
    test::capture_contract("replay", method, path, response);
    return json::parse(response.body);
  };
  capture("GET", "/api/replay");
  const auto started = capture("POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}});
  const auto id = started.at("replay").at("id").get<std::string>();
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
  capture("PUT", "/api/replay", {{"until", "10:00:01"}});
  capture("GET", "/api/replay/status");
  capture("GET", "/api/replay/account");
  capture("DELETE", "/api/replay");
  capture("GET", "/api/replay");
  capture("GET", "/api/replay/history/" + id);
  capture("GET", "/api/replay/history/" + id + "/trades");
}
}  // namespace

namespace {
TEST(ReplayRun, CommandActorDefaultsForOlderInputs) {
  server::TradingCommand command;
  command.actor = "agent";
  auto recorded = json(command);
  EXPECT_EQ(recorded.get<server::TradingCommand>().actor, "agent");
  recorded.erase("actor");
  EXPECT_EQ(recorded.get<server::TradingCommand>().actor, "unknown");
}

/// A run with one order an agent placed, recorded with or without driver 2's instants.
std::filesystem::path attributed_run(const test::RecordingFile& file, bool instants) {
  const auto journal = file.directory / (instants ? "run.jsonl" : "run-driver-1.jsonl");
  const test::ScriptedMarket market;
  md::RecordingReader reader(file.path);
  server::Desk::Options options;
  options.run_input = server::recording_input(file.path);
  options.replay = true;
  options.instant_batches = instants;
  options.paper_journal = journal;
  server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription, instants);
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    if (desk.market_time() == market.time + 4 * md::kNanosPerSecond) {
      server::TradingCommand order;
      order.actor = "agent";
      order.order = market.market("older-order");
      EXPECT_TRUE(command(desk, order, desk.market_time(), batch->received).decision.ok());
    }
  }
  return journal;
}
/// Copies `journal` with a new hash chain, removing every actor from the records `strip` picks.
std::filesystem::path without_actors(const std::filesystem::path& journal, const std::string& name,
                                     const std::function<bool(const trading::JournalRecord&)>& strip) {
  const std::function<void(json&)> erase = [&](json& value) {
    if (value.is_object()) value.erase("actor");
    if (value.is_structured()) {
      for (auto& child : value) erase(child);
    }
  };
  const auto copy = journal.parent_path() / name;
  auto output = trading::FileJournal::create(copy.string());
  for (const auto& record : trading::FileJournal::read(journal.string()).records) {
    auto payload = json::parse(record.payload);
    if (strip(record)) erase(payload);
    output->append(record.time, record.type, payload.dump());
  }
  return copy;
}

TEST(ReplayRun, JournalsWithoutActorsStillVerifyTheirOriginalHashes) {
  test::RecordingFile file;
  write_stream(file.path, true);
  // Builds before actors wrote no driver version.
  const auto older = without_actors(attributed_run(file, false), "older.jsonl", [](const auto&) { return true; });
  const auto recovery = trading::FileJournal::read(older.string());
  const auto restored = trading::TradingSession::recover(recovery);
  ASSERT_FALSE(restored.snapshot()->recent_fills.empty());
  EXPECT_EQ(restored.snapshot()->recent_fills.front().actor, "unknown");
  const auto verified = server::verify_run(older);
  EXPECT_TRUE(verified.matched) << verified.message;
  EXPECT_EQ(verified.head, recovery.head);
}

TEST(ReplayRun, AnActorRemovedFromOneRecordIsADifference) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const auto journal = attributed_run(file, true);
  ASSERT_TRUE(server::verify_run(journal).matched);
  const auto records = trading::FileJournal::read(journal.string()).records;
  const auto submit = std::find_if(records.begin(), records.end(), [](const auto& r) { return r.type == "submit"; });
  ASSERT_NE(submit, records.end());
  ASSERT_EQ(submit->actor, "agent");
  // Who placed the order cannot be erased from the order's record, nor from the
  // command's run input, the other record that names it.
  const auto order = without_actors(journal, "order.jsonl", [&](const auto& r) { return r.seq == submit->seq; });
  auto verified = server::verify_run(order);
  EXPECT_FALSE(verified.matched);
  EXPECT_NE(verified.message.find("First differing transaction " + std::to_string(submit->seq) + " (submit)"),
            std::string::npos) << verified.message;
  const auto input = std::find_if(records.begin(), records.end(), [](const auto& r) {
    return r.type == "run_input" && r.actor == "agent";
  });
  ASSERT_NE(input, records.end());
  const auto command = without_actors(journal, "command.jsonl", [&](const auto& r) { return r.seq == input->seq; });
  verified = server::verify_run(command);
  EXPECT_FALSE(verified.matched);
  EXPECT_NE(verified.message.find("First differing transaction " + std::to_string(std::min(input->seq, submit->seq))),
            std::string::npos) << verified.message;
  // Nor from every record of a run whose driver version postdates actors.
  verified = server::verify_run(without_actors(journal, "all.jsonl", [](const auto&) { return true; }));
  EXPECT_FALSE(verified.matched);
  EXPECT_NE(verified.message.find("First differing transaction 1 (session_start)"), std::string::npos) << verified.message;
}
}  // namespace

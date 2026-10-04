#include <benchmark/benchmark.h>

#include <filesystem>
#include <stdexcept>
#include <unistd.h>

#include "../tests/support/scripted_market.hpp"
#include "openport/providers/replay_batches.hpp"
#include "openport/providers/scenario.hpp"
#include "openport/server/desk.hpp"

namespace {
using namespace openport;

trading::TradingSession history(test::ScriptedMarket& market, std::int64_t fills) {
  trading::SessionConfig config;
  config.initial_cash = trading::Money::parse("1000000000");
  // Each round trip pays the spread and fees; the history must not stop at the loss limit.
  config.limits.max_daily_loss = trading::Money::parse("1000000000");
  trading::TradingSession session(config, market.time);
  market.seed(session, "4.00", "4.20", fills + 10);
  for (std::int64_t i = 0; i < fills; ++i) {
    const auto result = session.submit(market.market(std::to_string(i), 1,
        i % 2 == 0 ? trading::Side::Buy : trading::Side::Sell), market.time);
    if (!result.decision.ok()) throw std::runtime_error(result.decision.message);
  }
  return session;
}
void rest(test::ScriptedMarket& market, trading::TradingSession& session) {
  // A bid inside the price band, below the ask: it rests, keeping each batch on the
  // active transaction path without growing fill or order history.
  if (!session.submit(market.limit("resting", 1, "3.70"), market.time).decision.ok())
    throw std::runtime_error("The resting order was refused");
}
void quotes(benchmark::State& state, test::ScriptedMarket& market, trading::TradingSession& session) {
  for (auto _ : state) {
    ++market.observation;
    benchmark::DoNotOptimize(session.on_quotes({market.quote()}, {market.valuation()}, market.time));
  }
}
void BM_TradingOnQuotes(benchmark::State& state) {
  test::ScriptedMarket market;
  auto session = history(market, state.range(0));
  rest(market, session);
  quotes(state, market, session);
}
BENCHMARK(BM_TradingOnQuotes)->Arg(10)->Arg(1000)->Arg(10000)->Unit(benchmark::kMicrosecond);

// The same with a position open, so each batch also samples its trade review.
void BM_TradingOnQuotesHolding(benchmark::State& state) {
  test::ScriptedMarket market;
  auto session = history(market, state.range(0));
  rest(market, session);
  if (session.submit(market.market("held", 1), market.time).decision.code != trading::Reason::NONE ||
      session.snapshot()->positions.empty())
    throw std::runtime_error("The held position did not open");
  quotes(state, market, session);
}
BENCHMARK(BM_TradingOnQuotesHolding)->Arg(10)->Arg(1000)->Arg(10000)->Unit(benchmark::kMicrosecond);

// The two opening-rule input passes for one active account in a market batch.
// Other accounts retain long completed histories and one working opening each;
// setup and history growth are outside timing, as is journal I/O.
void BM_CounterExposureBatch(benchmark::State& state) {
  test::ScriptedMarket market;
  std::vector<trading::TradingSession> accounts;
  std::vector<std::string> names;
  for (std::int64_t i = 0; i < state.range(0); ++i) {
    accounts.push_back(history(market, state.range(1)));
    rest(market, accounts.back());
    names.push_back("other-" + std::to_string(i));
  }
  const std::vector<trading::Valuation> marks{market.valuation()};
  for (auto _ : state)
    for (int pass = 0; pass < 2; ++pass)
      for (std::size_t i = 0; i < accounts.size(); ++i)
        benchmark::DoNotOptimize(accounts[i].counter_exposures(names[i], market.time, marks));
  state.SetLabel("two input passes; other accounts / completed fills per account");
}
BENCHMARK(BM_CounterExposureBatch)->Args({4, 10})->Args({4, 1000})->Args({4, 10000})
    ->Args({8, 10000})->Unit(benchmark::kMicrosecond);

void BM_TradingSubmit(benchmark::State& state) {
  test::ScriptedMarket market;
  auto session = history(market, state.range(0));
  const auto order = market.market("measured");
  for (auto _ : state) benchmark::DoNotOptimize(session.submit(order, market.time));
}
// Exactly one fresh submit per seeded session, so history never grows across
// timed iterations and the idempotent retry shortcut is never measured.
BENCHMARK(BM_TradingSubmit)->Arg(10)->Arg(1000)->Arg(10000)->Iterations(1)->Repetitions(3)->Unit(benchmark::kMicrosecond);

// A submit beside many working orders, as a script that keeps a ladder of resting bids.
void BM_TradingSubmitWorking(benchmark::State& state) {
  test::ScriptedMarket market;
  trading::SessionConfig config;
  config.initial_cash = trading::Money::parse("1000000000");
  config.limits.aggregate = {1e15, 1e15};
  config.limits.per_underlying = {1e15, 1e15};
  trading::TradingSession session(config, market.time);
  market.seed(session);
  for (std::int64_t i = 0; i < state.range(0); ++i)
    if (!session.submit(market.limit("rest-" + std::to_string(i), 1, "3.70"), market.time).decision.ok())
      throw std::runtime_error("A resting order was refused");
  std::int64_t next = 0;
  for (auto _ : state) benchmark::DoNotOptimize(session.submit(market.limit("measured-" + std::to_string(next++), 1, "3.70"), market.time));
}
BENCHMARK(BM_TradingSubmitWorking)->Arg(10)->Arg(100)->Arg(700)->Unit(benchmark::kMicrosecond);

struct ScenarioFile {
  std::filesystem::path directory;
  std::filesystem::path file;
  providers::Scenario scenario;
  ScenarioFile() {
    auto pattern = (std::filesystem::temp_directory_path() / "openport-bench-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("Cannot create benchmark input");
    directory = pattern;
    file = directory / "scenario.oprec";
    scenario = providers::builtin_scenarios().front();
    scenario.symbols = {"SPX"};
    providers::write_scenario_recording(file, scenario, scenario.date, scenario.seed);
  }
  ~ScenarioFile() { std::error_code error; std::filesystem::remove_all(directory, error); }
};
void BM_DeterministicScenario(benchmark::State& state) {
  const ScenarioFile input;
  double hours = 0;
  for (auto _ : state) {
    md::RecordingReader reader(input.file);
    server::Desk::Options options;
    options.replay = true;
    server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    providers::ReplayBatches batches(reader, reader.header().subscription);
    while (const auto batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
    hours = static_cast<double>(desk.market_time() - reader.header().started) / (3600.0 * md::kNanosPerSecond);
    benchmark::DoNotOptimize(desk.trading_view());
  }
  state.counters["market_hours_per_second"] = benchmark::Counter(
      hours * static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  state.SetLabel("simulated SPX; analytics and flat paper account; no journal I/O");
}
BENCHMARK(BM_DeterministicScenario)->Iterations(1)->UseRealTime()->Unit(benchmark::kSecond);
}  // namespace

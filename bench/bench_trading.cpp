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
  trading::TradingSession session(config, market.time);
  market.seed(session, "4.00", "4.20", fills + 10);
  for (std::int64_t i = 0; i < fills; ++i) {
    const auto result = session.submit(market.market(std::to_string(i), 1,
        i % 2 == 0 ? trading::Side::Buy : trading::Side::Sell), market.time);
    if (!result.decision.ok()) throw std::runtime_error(result.decision.message);
  }
  return session;
}
void BM_TradingOnQuotes(benchmark::State& state) {
  test::ScriptedMarket market;
  auto session = history(market, state.range(0));
  // A resting order keeps this on the active transaction path without growing
  // fill or order history during the measurement.
  session.submit(market.limit("resting", 1, "1.00"), market.time);
  for (auto _ : state) {
    ++market.observation;
    benchmark::DoNotOptimize(session.on_quotes({market.quote()}, {market.valuation()}, market.time));
  }
}
BENCHMARK(BM_TradingOnQuotes)->Arg(10)->Arg(1000)->Arg(10000)->Unit(benchmark::kMicrosecond);

void BM_TradingSubmit(benchmark::State& state) {
  test::ScriptedMarket market;
  auto session = history(market, state.range(0));
  const auto order = market.market("measured");
  for (auto _ : state) benchmark::DoNotOptimize(session.submit(order, market.time));
}
// Exactly one fresh submit per seeded session, so history never grows across
// timed iterations and the idempotent retry shortcut is never measured.
BENCHMARK(BM_TradingSubmit)->Arg(10)->Arg(1000)->Arg(10000)->Iterations(1)->Repetitions(3)->Unit(benchmark::kMicrosecond);

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

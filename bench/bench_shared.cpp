#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <string>

#include "openport/trading/shared.hpp"

namespace {
using namespace openport::trading;

// Repeatable, scattered reads, with setup and key construction outside timing.
std::array<std::size_t, 1024> indices(std::size_t count) {
  std::array<std::size_t, 1024> out{};
  std::uint32_t seed = 7;
  for (auto& index : out) {
    seed = seed * 1664525U + 1013904223U;
    index = seed % count;
  }
  return out;
}
void BM_SharedVectorRead(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  SharedVector<std::size_t> values;
  for (std::size_t i = 0; i < count; ++i) values.push_back(i);
  const auto reads = indices(count);
  for (auto _ : state)
    for (const auto index : reads) {
      auto value = values[index];
      benchmark::DoNotOptimize(value);
    }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(reads.size()));
}
BENCHMARK(BM_SharedVectorRead)->Arg(10)->Arg(1000)->Arg(50000);

void BM_SharedMapFind(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  SharedMap<std::string, std::size_t> values;
  for (std::size_t i = 0; i < count; ++i) values.emplace(std::to_string(i), i);
  std::array<std::string, 1024> keys;
  const auto reads = indices(count);
  for (std::size_t i = 0; i < keys.size(); ++i) keys[i] = std::to_string(reads[i]);
  for (auto _ : state)
    for (const auto& key : keys) {
      auto value = values.find(key)->second;
      benchmark::DoNotOptimize(value);
    }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(keys.size()));
}
BENCHMARK(BM_SharedMapFind)->Arg(10)->Arg(1000)->Arg(50000);
}  // namespace

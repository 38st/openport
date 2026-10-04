#include <benchmark/benchmark.h>

#include "../src/trading/margin_detail.hpp"

namespace {
using namespace openport;

std::vector<trading::MarginLeg> mixed_book(std::int64_t count) {
  std::vector<trading::MarginLeg> legs;
  const auto first = *md::parse_osi("SPY261022P00500000");
  const auto later = *md::parse_osi("SPY261029P00500000");
  for (std::int64_t i = 0; i < count; ++i) {
    auto contract = i % 3 == 0 ? later : first;
    contract.type = i % 2 == 0 ? pricing::OptionType::Call : pricing::OptionType::Put;
    contract.strike = 450.0 + static_cast<double>(i);
    const bool shorted = i % 4 < 2;
    legs.push_back({contract, shorted ? -2 : 2, shorted ? trading::Money::parse("600.000002") : trading::Money{}, 500.0});
  }
  return legs;
}
void BM_MarginPairing(benchmark::State& state) {
  const auto legs = mixed_book(state.range(0));
  for (auto _ : state) benchmark::DoNotOptimize(trading::detail::pairing_margin_requirement(legs));
}
void BM_MarginJoint(benchmark::State& state) {
  const auto legs = mixed_book(state.range(0));
  for (auto _ : state) benchmark::DoNotOptimize(trading::margin_requirement(legs));
}
void BM_MarginJointLargeQuantity(benchmark::State& state) {
  auto legs = mixed_book(state.range(0));
  for (auto& leg : legs) { leg.quantity *= 10000; leg.value = leg.value * 10000; }
  for (auto _ : state) benchmark::DoNotOptimize(trading::margin_requirement(legs));
}
BENCHMARK(BM_MarginPairing)->Arg(14)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginJoint)->Arg(14)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginJointLargeQuantity)->Arg(14)->Arg(80)->Unit(benchmark::kMicrosecond);
}  // namespace

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
std::vector<trading::MarginLeg> fragmented_book() {
  std::vector<trading::MarginLeg> legs;
  const auto first = *md::parse_osi("SPY261022P00500000");
  const auto second = *md::parse_osi("SPY261029P00500000");
  const auto third = *md::parse_osi("SPY261105P00500000");
  for (int i = 0; i < 40; ++i) {
    auto shorted = i % 3 == 0 ? first : i % 3 == 1 ? second : third;
    shorted.type = i % 2 == 0 ? pricing::OptionType::Call : pricing::OptionType::Put;
    shorted.strike = 460.0 + i * 2;
    auto cover = shorted;
    cover.strike += shorted.type == pricing::OptionType::Call ? 1 : -1;
    legs.push_back({shorted, -1, trading::Money::parse("250"), 500.0});
    legs.push_back({cover, 1, {}, 500.0});
  }
  return legs;
}
// Many small pairs/pools and distinct strikes force the bounded neighbourhood
// search to do substantially more work than the ordinary 80-leg book.
void BM_MarginPairingFragmented(benchmark::State& state) {
  const auto legs = fragmented_book();
  for (auto _ : state) benchmark::DoNotOptimize(trading::detail::pairing_margin_requirement(legs));
}
void BM_MarginJointFragmented(benchmark::State& state) {
  const auto legs = fragmented_book();
  for (auto _ : state) benchmark::DoNotOptimize(trading::margin_requirement(legs));
}
BENCHMARK(BM_MarginPairing)->Arg(14)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginJoint)->Arg(14)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginJointLargeQuantity)->Arg(14)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginPairingFragmented)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginJointFragmented)->Arg(80)->Unit(benchmark::kMicrosecond);
}  // namespace

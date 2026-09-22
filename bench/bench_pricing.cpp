#include <benchmark/benchmark.h>

#include <cmath>
#include <random>
#include <vector>

#include "openport/pricing/binomial.hpp"
#include "openport/pricing/black.hpp"
#include "openport/pricing/implied_vol.hpp"

namespace {

using openport::pricing::OptionType;

struct Quote {
  OptionType type;
  double forward, strike, expiry, vol, price;
};

// A realistic chain: strikes within +-3 standard deviations, one day to one year.
std::vector<Quote> make_quotes(std::size_t n) {
  std::mt19937_64 rng(42);
  std::uniform_real_distribution<double> z(-3.0, 3.0);
  std::uniform_real_distribution<double> expiry(1.0 / 365.0, 1.0);
  std::uniform_real_distribution<double> vol(0.2, 1.2);
  std::vector<Quote> quotes;
  quotes.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = expiry(rng);
    const double v = vol(rng);
    const double k = 100.0 * std::exp(-z(rng) * v * std::sqrt(t));
    const OptionType type = k >= 100.0 ? OptionType::Call : OptionType::Put;
    quotes.push_back({type, 100.0, k, t, v, openport::pricing::black_price(type, 100.0, k, t, v)});
  }
  return quotes;
}

void BM_BlackPrice(benchmark::State& state) {
  const auto quotes = make_quotes(4096);
  std::size_t i = 0;
  for (auto _ : state) {
    const Quote& q = quotes[i++ & 4095];
    benchmark::DoNotOptimize(
        openport::pricing::black_price(q.type, q.forward, q.strike, q.expiry, q.vol));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BlackPrice);

void BM_BlackGreeks(benchmark::State& state) {
  const auto quotes = make_quotes(4096);
  std::size_t i = 0;
  for (auto _ : state) {
    const Quote& q = quotes[i++ & 4095];
    benchmark::DoNotOptimize(
        openport::pricing::black_greeks(q.type, q.forward, q.strike, q.expiry, q.vol));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BlackGreeks);

void BM_ImpliedVol(benchmark::State& state) {
  const auto quotes = make_quotes(4096);
  std::size_t i = 0;
  std::int64_t iterations = 0;
  for (auto _ : state) {
    const Quote& q = quotes[i++ & 4095];
    auto iv =
        openport::pricing::implied_vol_black(q.price, q.type, q.forward, q.strike, q.expiry);
    iterations += iv.iterations;
    benchmark::DoNotOptimize(iv);
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["newton_iters"] =
      benchmark::Counter(static_cast<double>(iterations), benchmark::Counter::kAvgIterations);
}
BENCHMARK(BM_ImpliedVol);

void BM_AmericanLeisenReimer(benchmark::State& state) {
  const openport::pricing::BsmInputs in{OptionType::Put, 100.0, 105.0, 0.5, 0.04, 0.01, 0.3};
  const int steps = static_cast<int>(state.range(0));
  for (auto _ : state) {
    benchmark::DoNotOptimize(openport::pricing::binomial_price(
        in, openport::pricing::ExerciseStyle::American,
        openport::pricing::TreeMethod::LeisenReimer, steps));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AmericanLeisenReimer)->Arg(101)->Arg(501)->Arg(1001)->Unit(benchmark::kMicrosecond);

}  // namespace

#pragma once

#ifdef OPENPORT_SANITIZER_BUILD
#include "providers/scenario_sampling.hpp"
#endif

namespace openport::test {
constexpr bool sanitizer_build() {
#ifdef OPENPORT_SANITIZER_BUILD
  return true;
#else
  return false;
#endif
}

template <class T> constexpr T sanitizer_scale(T full, T sanitized) {
  return sanitizer_build() ? sanitized : full;
}

// GoogleTest runs one test at a time in each process. Keep this scope alive until
// its generators, feeds, backtests and independent verification have all joined;
// their worker threads share the same sampling policy. Regular builds are a no-op.
// Sampled journals are temporary fixtures and must be verified inside this scope.
class SanitizerScenarioScale {
 public:
  SanitizerScenarioScale() = default;
  SanitizerScenarioScale(const SanitizerScenarioScale&) = delete;
  SanitizerScenarioScale& operator=(const SanitizerScenarioScale&) = delete;
  ~SanitizerScenarioScale() {
#ifdef OPENPORT_SANITIZER_BUILD
    providers::detail::sample_scenario_snapshots.store(previous_, std::memory_order_relaxed);
#endif
  }

 private:
#ifdef OPENPORT_SANITIZER_BUILD
  bool previous_ = providers::detail::sample_scenario_snapshots.exchange(true, std::memory_order_relaxed);
#endif
};
}  // namespace openport::test

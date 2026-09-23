#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "openport/md/time.hpp"

namespace openport::providers {

/// The provider name the demo market records under.
inline constexpr std::string_view kDemoProvider = "demo";

/// True for the demo market, including its replay ("replay (demo)").
[[nodiscard]] bool simulated_provider(std::string_view name) noexcept;

/// A simulated trading day in SPX and SPY options, for trying the terminal when
/// nothing trades: generated prices, not market data. The same date and seed give
/// the same day on the same platform.
struct DemoOptions {
  md::Date date{2026, 9, 16};
  std::uint64_t seed = 20260916;
};

/// Writes the day as a recording that ReplayProvider plays, under the provider
/// name "demo": 15-second snapshots from the 09:30 ET open to 16:15, when the last
/// SPY options stop trading. SPX falls through the morning and rallies in the
/// afternoon, implied volatility rises as it falls, and each chain lists five
/// expiries from 0DTE to next month with open interest. Throws if `path` exists.
void write_demo_recording(const std::filesystem::path& path, const DemoOptions& options = {});

}  // namespace openport::providers

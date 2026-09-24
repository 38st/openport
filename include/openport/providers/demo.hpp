#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "openport/md/time.hpp"

namespace openport::providers {

/// The provider name the demo market records under.
inline constexpr std::string_view kDemoProvider = "demo";

/// True for the demo market, including its replay ("replay (demo)").
[[nodiscard]] bool simulated_provider(std::string_view name) noexcept;

/// The kinds of simulated day the demo market plays.
enum class DemoDay { Reversal, Trend, Chop, Selloff, Overnight };

/// A demo day as the Replay page lists it.
struct DemoInfo {
  DemoDay day = DemoDay::Reversal;
  std::string id;           ///< "reversal", "trend", "chop", "selloff" or "overnight"
  std::string title;
  std::string description;  ///< One sentence on how the day goes.
  md::Date date;            ///< Its trading date.
  std::vector<std::string> symbols;
  md::Timestamp started = 0;  ///< Its first event.
};

/// Every demo day, the default (the reversal) first.
[[nodiscard]] const std::vector<DemoInfo>& demo_days();
[[nodiscard]] const DemoInfo* find_demo_day(std::string_view id);

/// A simulated trading day in SPX, SPY and QQQ options, for trying the terminal
/// when nothing trades: generated prices, not market data. The same day, date and
/// seed give the same recording on the same platform.
struct DemoOptions {
  DemoDay day = DemoDay::Reversal;
  std::optional<md::Date> date;  ///< Another trading date to play the day on.
  std::uint64_t seed = 0;        ///< Zero keeps the day's own.
};

/// Writes the day as a recording that ReplayProvider plays, under the provider name
/// "demo". Regular days run in 15-second snapshots from the 09:30 ET open to 16:15,
/// when the last SPY and QQQ options stop trading, with five expiries a chain from
/// 0DTE to next month and open interest; the index follows the day's script and
/// implied volatility rises as it falls. The overnight day is SPX options alone in
/// Cboe's global trading hours, 20:15 to 09:25 ET, in 60-second snapshots, with the
/// index frozen at its close. Throws if `path` exists or the date does not trade.
void write_demo_recording(const std::filesystem::path& path, const DemoOptions& options = {});

}  // namespace openport::providers

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "openport/md/time.hpp"

namespace openport::providers {

struct ScenarioEvent {
  std::string type;  ///< gap, crush, spike or pin
  std::string at;    ///< HH:MM ET; gap has no time
  double move = 0;   ///< log return, gap or spike
  double iv = 0;     ///< absolute IV change, crush or spike
  double strike = 0; ///< SPX strike, pin
};

struct Scenario {
  std::string id, title, description, goal;
  std::vector<std::string> symbols;
  bool overnight = false;
  md::Date date;
  std::uint64_t seed = 0;
  int generator = 1;
  std::vector<std::pair<double, double>> drift;  ///< fraction -> cumulative log return
  double volatility = 0, iv_shift = 0, spot_vol = 0;
  std::vector<ScenarioEvent> events;
};

/// Strict version 1 parser. Errors include the file and field.
[[nodiscard]] Scenario parse_scenario(std::string_view source, const std::filesystem::path& file);
[[nodiscard]] Scenario read_scenario(const std::filesystem::path& file);
/// Built-ins first, user files after them, replacing built-ins with the same id.
/// Invalid built-ins throw; invalid user files are reported and skipped.
[[nodiscard]] std::vector<Scenario> load_scenarios(const std::filesystem::path& user = {},
    const std::function<void(const std::string&)>& log = {});
[[nodiscard]] const std::vector<Scenario>& builtin_scenarios();
[[nodiscard]] md::Timestamp scenario_open(const Scenario& scenario, md::Date date);
[[nodiscard]] md::Timestamp scenario_close(const Scenario& scenario, md::Date date);
/// Overnight HH:MM >= 20:15 belongs to the evening before the trading date.
[[nodiscard]] md::Timestamp scenario_time(std::string_view time, md::Date date, bool overnight);
void write_scenario_recording(const std::filesystem::path& path, const Scenario& scenario,
                              md::Date date, std::uint64_t seed);

}  // namespace openport::providers

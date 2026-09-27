#pragma once

#include <filesystem>
#include <string>

#include "openport/providers/scenario.hpp"
#include "openport/server/desk.hpp"

namespace openport::server {

[[nodiscard]] std::string recording_input(const std::filesystem::path& file);
[[nodiscard]] std::string scenario_input(const providers::Scenario& scenario, md::Date date, std::uint64_t seed);

struct RunVerification {
  bool matched = false;
  std::uint64_t transactions = 0;
  std::string head;
  trading::Money equity;
  std::string message;
};
/// Opens input read-only and regenerates the complete recorded prefix in memory.
/// Offline only: temporarily restores the recorded process calendar.
/// Missing/changed input is reported separately from a transaction mismatch.
[[nodiscard]] RunVerification verify_run(const std::filesystem::path& journal);

}  // namespace openport::server

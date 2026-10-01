#pragma once

#include <filesystem>
#include <string>

#include "openport/providers/scenario.hpp"
#include "openport/server/desk.hpp"

namespace openport::server {

[[nodiscard]] std::string recording_input(const std::filesystem::path& file);
[[nodiscard]] std::string scenario_input(const providers::Scenario& scenario, md::Date date, std::uint64_t seed);
/// The run a recording_input or scenario_input describes, under the run's history id.
[[nodiscard]] RunIdentity run_identity(std::string_view input, std::string id);

struct RunVerification {
  bool matched = false;
  std::uint64_t transactions = 0;
  std::string head;
  trading::Money equity;
  std::string message;
  bool cut = false;  ///< The journal ends part way through its last operation, as a crash leaves it.
};
/// Opens input read-only and regenerates the complete recorded prefix in memory,
/// including one a crash cut off part way through an operation.
/// Offline only: temporarily restores the recorded process calendar.
/// Missing/changed input is reported separately from a transaction mismatch.
[[nodiscard]] RunVerification verify_run(const std::filesystem::path& journal);

}  // namespace openport::server

#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "openport/server/api.hpp"

namespace openport::test {
/// Python's offline contract check requests real handler responses from these
/// fixtures. Normal C++ runs make no files; no listener or provider is needed.
inline void capture_contract(std::string_view family, std::string_view method, std::string_view path,
                              const server::ApiResponse& response) {
  const auto* directory = std::getenv("OPENPORT_CONTRACT_OUTPUT");
  if (!directory) return;
  nlohmann::json body = response.content_type.starts_with("application/json")
      ? nlohmann::json::parse(response.body) : nlohmann::json(response.body);
  const nlohmann::json row = {{"method", method}, {"path", path}, {"status", response.status},
                              {"content_type", response.content_type}, {"body", std::move(body)}};
  std::ofstream output(std::filesystem::path(directory) / (std::string(family) + ".jsonl"), std::ios::app);
  output << row.dump() << '\n';
  if (!output) throw std::runtime_error("Cannot capture contract fixture");
}
}  // namespace openport::test

#pragma once

#include <nlohmann/json.hpp>

namespace openport::trading::detail {

/// The change from one state's JSON to the next, for schema 3 journal records.
/// Each node replaces a value ({"v": value}), patches an object's keys
/// ({"o": {key: node}, "d": [removed keys]}) or patches an array's elements
/// ({"a": {"index": node}, "n": new length}). Unchanged values are left out, so a
/// transaction that changes a few fields records a few fields, however long the
/// account's history. Mostly rewritten arrays are recorded whole.
[[nodiscard]] nlohmann::json state_delta(const nlohmann::json& before, const nlohmann::json& after);

/// Applies a delta made by state_delta. Throws std::invalid_argument when a node
/// does not fit the value it patches.
void apply_state_delta(nlohmann::json& state, const nlohmann::json& delta);

}  // namespace openport::trading::detail

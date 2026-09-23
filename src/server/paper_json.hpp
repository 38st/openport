#pragma once
#include <nlohmann/json.hpp>
#include "openport/server/api.hpp"

namespace openport::server {
nlohmann::json trading_status_json(const TradingStatus& status);
/// Each account's ID, name and trading status, for status and ticks.
nlohmann::json account_ticks_json(const EngineStatus& status);
std::optional<ApiResponse> paper_read(const ApiRequest& request, const MetricsSource& source);
}

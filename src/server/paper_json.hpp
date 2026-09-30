#pragma once
#include <nlohmann/json.hpp>
#include "openport/server/api.hpp"
#include "openport/trading/types.hpp"

namespace openport::server {
nlohmann::json trading_status_json(const TradingStatus& status);
/// Each account's ID, name and trading status, for status and ticks.
nlohmann::json account_ticks_json(const EngineStatus& status);
std::optional<ApiResponse> paper_read(const ApiRequest& request, const MetricsSource& source);
/// P&L by Greek in dollars, to the cent, with parts that add up to the total.
nlohmann::json attribution_json(const trading::Attribution& attribution);
}

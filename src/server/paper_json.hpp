#pragma once
#include <nlohmann/json.hpp>
#include "openport/server/api.hpp"

namespace openport::server {
nlohmann::json trading_status_json(const TradingStatus& status);
std::optional<ApiResponse> paper_read(const ApiRequest& request, const MetricsSource& source);
}

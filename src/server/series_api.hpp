#pragma once
#include <map>
#include <nlohmann/json.hpp>
#include "openport/server/api.hpp"
namespace openport::server {
ApiResponse series_response(const MetricsSource& source, const std::string& symbol,
                            const std::map<std::string, std::string>& query);
nlohmann::json series_status_json(const MetricsSource& source);
void add_volatility_history(nlohmann::json& response, const MetricsSource& source,
                            const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics);
}

#pragma once
#include "openport/server/api.hpp"
namespace openport::server {
std::optional<ApiResponse> playbook_read(const ApiRequest& request, const MetricsSource& source);
bool playbook_write(const ApiRequest& request, MetricsSource& source, ApiCompletion complete);
}

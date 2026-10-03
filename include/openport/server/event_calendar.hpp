#pragma once

#include "openport/server/api.hpp"
#include "openport/trading/events.hpp"

namespace openport::server {
/// The optional daemon catalogue is a read-scope import source, independent of
/// each account's saved rules. Returns no response for other routes.
[[nodiscard]] std::optional<ApiResponse> event_calendar_read(const ApiRequest& request,
    const std::vector<trading::PlanEvent>& events);
}  // namespace openport::server

#pragma once

#include <string>
#include <string_view>

#include "openport/server/engine.hpp"

namespace openport::server {

struct ApiRequest {
  std::string method = "GET";
  std::string target;  ///< path and query, e.g. "/api/underlyings/SPX/chain?expiry=2026-10-05PM"
};

struct ApiResponse {
  int status = 200;
  std::string body;  ///< JSON
};

/// Routes:
///   GET /api/status
///   GET /api/underlyings/{symbol}/summary
///   GET /api/underlyings/{symbol}/chain?expiry={id}[&window=0.1]
///   GET /api/underlyings/{symbol}/exposure[?expiries=8][&window=0.08]
///   GET /api/underlyings/{symbol}/surface[?expiries=12][&window=0.2]
/// Expiry ids are the date plus settlement, e.g. "2026-10-16AM", because SPX
/// (morning settlement) and SPXW (afternoon) can expire on the same day.
/// OEX and XEO additionally carry -OEX or -XEO to distinguish exercise styles.
[[nodiscard]] ApiResponse handle_api(const ApiRequest& request, const MetricsSource& source);

/// The small message pushed to every WebSocket client each second, so the UI knows
/// when data changed without polling everything.
[[nodiscard]] std::string tick_message(const MetricsSource& source);

}  // namespace openport::server

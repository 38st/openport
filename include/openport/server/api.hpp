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
/// Status and ticks include market {open, note, next_open}; this uses wall-clock
/// regular-session hours, while as_of remains the market data's timestamp.
/// next_open uses the same UTC timestamp format as as_of; null while open.
/// All four underlying views include spot_source ("quote", "parity", or null).
/// Summary includes american_approximation (any priced American expiry) and
/// coverage {options, quoted, priced, open_interest} over live standard contracts.
/// summary.expiries[] and chain.expiry include style ("european"/"american") and
/// the same coverage counts. Quoted/OI counts mean received, including real zeros;
/// priced means the option has its own valid IV, not just its strike's smile IV.
/// Chain bid/ask/mid and oi are null when never received; invalid values are null.
/// Exposure summaries include oi_coverage: received OI / valid-IV live options,
/// or null for an empty set. This covers the full exposure, regardless of display
/// window/expiry filters. Only valid-IV options with usable OI contribute risk.
[[nodiscard]] ApiResponse handle_api(const ApiRequest& request, const MetricsSource& source);

/// The small message pushed to every WebSocket client each second, so the UI knows
/// when data changed without polling everything.
[[nodiscard]] std::string tick_message(const MetricsSource& source);

}  // namespace openport::server

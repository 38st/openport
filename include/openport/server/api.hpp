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
/// Each status/tick underlyings[] entry also includes session {name, open, note}
/// at wall-clock time, where name is regular/curb/global/closed for that product.
/// The top-level market remains the regular-session calendar for compatibility.
/// All four underlying views include spot_source ("quote", "parity", or null).
/// Summary includes american_approximation (any priced American expiry without
/// de-Americanisation) and
/// coverage {options, quoted, priced, open_interest} over live standard contracts.
/// summary.expiries[] and chain.expiry include style ("european"/"american") and
/// the same coverage counts. Quoted/OI counts mean received, including real zeros;
/// priced means the option has its own valid IV, not just its strike's smile IV.
/// Each expiry includes rate_source: "parity" (this expiry's fitted rate), "term"
/// (borrowed from this underlying's longer European expiries), "curve" (another
/// underlying's European parity curve), or "assumed" (flat fallback). rate_fitted
/// remains true only for "parity". rate_curve_symbol names the curve (e.g. "SPX")
/// for "curve", otherwise null. deamericanized reports EEP removal with an LR
/// tree. Chain bid/ask/mid remain the raw market quotes; iv/bid_iv/ask_iv use
/// premium-adjusted prices. Each option includes eep, the per-unit price premium
/// used for those solves (null when not computed, zero when computed as zero).
/// Greeks remain European Black-76 Greeks at the final smile IV, accurate OTM.
/// Chain bid/ask/mid and oi are null when never received; invalid values are null.
/// Exposure summaries include oi_coverage: received OI / options at finite-smile-IV strikes,
/// or null for an empty set. This covers the full exposure, regardless of display
/// window/expiry filters. Every side at those strikes with usable OI contributes risk, even without
/// its own IV.
[[nodiscard]] ApiResponse handle_api(const ApiRequest& request, const MetricsSource& source);

/// The small message pushed to every WebSocket client each second, so the UI knows
/// when data changed without polling everything.
[[nodiscard]] std::string tick_message(const MetricsSource& source);

}  // namespace openport::server

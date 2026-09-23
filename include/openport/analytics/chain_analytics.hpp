#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "openport/analytics/chain_book.hpp"
#include "openport/analytics/forward.hpp"

namespace openport::analytics {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// OpenPort's own numbers for one option: the same for every provider.
/// Greeks are per unit of underlying (multiply by the contract multiplier for a
/// position), with delta and gamma taken with respect to spot.
struct OptionMetrics {
  md::InstrumentId id = kNoInstrument;
  double bid = 0.0;
  double ask = 0.0;
  double mid = kNaN;
  double iv = kNaN;  ///< implied from the mid
  double bid_iv = kNaN;
  double ask_iv = kNaN;
  double delta = kNaN;
  double gamma = kNaN;  ///< per $1 of spot
  double vega = kNaN;   ///< per vol point
  double theta = kNaN;  ///< per calendar day
  double vanna = kNaN;  ///< change in delta per vol point
  double open_interest = 0.0;
  double vendor_iv = kNaN;
};

struct StrikeMetrics {
  double strike = 0.0;
  double iv = kNaN;  ///< smile IV, taken from the out-of-the-money side
  OptionMetrics call;
  OptionMetrics put;
  double gex = 0.0;  ///< dollars of hedging per 1% move in spot (calls add, puts subtract)
  double vex = 0.0;  ///< dollars of delta per vol point (calls add, puts subtract)
};

struct SliceMetrics {
  md::Date expiry;
  md::Timestamp expiry_time = 0;
  std::string root;
  double years = 0.0;
  ForwardEstimate forward;
  double atm_iv = kNaN;
  double gex = 0.0;
  double vex = 0.0;
  std::vector<StrikeMetrics> strikes;
};

/// Exposure uses the common open-interest convention: dealers are assumed long the
/// calls and short the puts that customers hold. That is a modelling convention,
/// not knowledge of anyone's actual positions.
struct ExposureSummary {
  double gex = 0.0;
  double vex = 0.0;
  double gamma_flip = kNaN;  ///< spot level where total GEX changes sign, nearest to spot
  double call_wall = kNaN;   ///< strike with the largest positive GEX, all expiries combined
  double put_wall = kNaN;    ///< strike with the most negative GEX, all expiries combined
};

struct UnderlyingMetrics {
  std::string symbol;
  double spot = 0.0;
  md::Timestamp as_of = 0;
  std::uint64_t version = 0;  ///< version of the book this was computed from
  std::vector<SliceMetrics> slices;
  ExposureSummary exposure;
  int options_priced = 0;
  double compute_ms = 0.0;
};

struct AnalyticsOptions {
  int parity_strikes = 12;      ///< strikes nearest the money used to fit the forward
  double flip_range = 0.10;     ///< search for the gamma flip within +-10% of spot
  int flip_steps = 81;
  double fallback_rate = 0.04;  ///< discount-rate assumption when parity cannot fit one

  /// Expiries shorter than this take their discount rate from the longer expiries
  /// (the median of their fitted rates): over a few days D is within a basis point
  /// of 1, so bid/ask noise swamps the parity slope that would otherwise estimate it.
  double min_days_for_rate = 30.0;

  /// Exposure (GEX, VEX, gamma flip) uses at least this much time to expiry. Local
  /// gamma of an option minutes from expiry explodes but only holds over a tiny move,
  /// so it would drown out everything else. Displayed Greeks use the true time.
  double exposure_min_days = 0.5;
};

/// Prices one underlying's whole chain as of `as_of` (the data's own time, not the
/// wall clock, so delayed feeds get the right time to expiry).
///
/// Per expiry: the forward and discount factor come from put-call parity; each
/// option's IV is implied from its mid with Black-76 on that forward; each strike's
/// smile IV comes from its out-of-the-money side, and both sides' Greeks use it so
/// they stay consistent. American options are priced as European here, which is
/// accurate for the out-of-the-money side that the smile uses.
[[nodiscard]] UnderlyingMetrics analyze(const UnderlyingBook& book, const ChainBook& chain,
                                        md::Timestamp as_of, const AnalyticsOptions& options = {});

}  // namespace openport::analytics

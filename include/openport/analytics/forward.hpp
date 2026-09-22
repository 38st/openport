#pragma once

#include <span>

namespace openport::analytics {

/// One strike where both the call and the put have two-sided quotes.
struct ParityPoint {
  double strike = 0.0;
  double call_mid = 0.0;
  double put_mid = 0.0;
  double weight = 1.0;  ///< usually 1 / (call spread + put spread)^2
};

struct ForwardEstimate {
  double forward = 0.0;
  double discount = 1.0;         ///< discount factor to expiry, exp(-rT)
  int points = 0;                ///< strikes used in the final fit
  bool fitted_discount = false;  ///< false when the discount factor was assumed
  bool ok = false;
};

/// Implied forward and discount factor from put-call parity.
///
/// For European options C - P = D (F - K), so C - P is a straight line in K with
/// slope -D and intercept D F. A weighted least-squares fit over near-the-money
/// strikes recovers both, so no interest-rate or dividend feed is needed: the
/// market's own prices carry the carry. Points more than three weighted standard
/// deviations off the first fit are dropped and the line refitted once.
///
/// With too few strikes, or a discount factor outside plausible bounds (rates
/// between -5% and 20%), D is assumed to be exp(-fallback_rate * T) and only F is
/// estimated. For American options parity holds only approximately; near-the-money
/// strikes keep the early-exercise premium small.
[[nodiscard]] ForwardEstimate implied_forward(std::span<const ParityPoint> points,
                                              double expiry_years, double fallback_rate = 0.0);

/// The forward alone, for a discount factor known from elsewhere (for example the
/// rate term structure fitted on longer expiries): the weighted mean of
/// K + (C - P) / D, after dropping points more than three weighted standard
/// deviations from a first pass.
[[nodiscard]] ForwardEstimate implied_forward_given_discount(std::span<const ParityPoint> points,
                                                             double discount);

}  // namespace openport::analytics

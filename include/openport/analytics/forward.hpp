#pragma once

#include <span>

namespace openport::analytics {

/// One strike where both the call and the put have two-sided quotes.
struct ParityPoint {
  double strike = 0.0;
  double call_mid = 0.0;
  double put_mid = 0.0;
  double weight = 1.0;  ///< inverse squared spread; capped absolutely and relative to median
};

struct ForwardEstimate {
  double forward = 0.0;
  double discount = 1.0;         ///< discount factor to expiry, exp(-rT)
  int points = 0;                ///< strikes used in the final fit
  bool fitted_discount = false;  ///< true only when this expiry fitted its own discount
  bool ok = false;
};

/// Implied forward and discount factor from put-call parity.
///
/// For European options C - P = D (F - K), so C - P is a straight line in K with
/// slope -D and intercept D F. A weighted least-squares fit over near-the-money
/// strikes recovers both, so no interest-rate or dividend feed is needed: the
/// market's own prices carry the carry. Weights are capped; a weighted median of
/// implied forwards and a MAD residual scale reject stale quotes before fitting.
///
/// With too few strikes, or a discount factor outside plausible bounds (rates
/// between -5% and 20%), D is assumed to be exp(-fallback_rate * T) and only F is
/// estimated. Use this slope fit only for European prices. American analytics
/// supplies a European curve or assumed discount and removes early-exercise
/// premium before refitting with implied_forward_given_discount.
[[nodiscard]] ForwardEstimate implied_forward(std::span<const ParityPoint> points,
                                              double expiry_years, double fallback_rate = 0.0);

/// The forward alone, for a discount factor known from elsewhere (for example the
/// rate term structure fitted on longer expiries): the weighted mean of
/// K + (C - P) / D, after weighted-median/MAD rejection with capped weights. Set weighted_median
/// to return the weighted median instead (American first-pass forward).
[[nodiscard]] ForwardEstimate implied_forward_given_discount(std::span<const ParityPoint> points,
                                                             double discount,
                                                             bool weighted_median = false);

}  // namespace openport::analytics

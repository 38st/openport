#pragma once

#include <cstdint>
#include <span>

#include "openport/pricing/black.hpp"

namespace openport::pricing {

enum class ExerciseStyle : std::uint8_t { European, American };

enum class TreeMethod : std::uint8_t {
  CoxRossRubinstein,  ///< u = exp(vol * sqrt(dt)); converges like 1/n with odd-even wobble
  LeisenReimer,       ///< Peizer-Pratt inversion; converges like 1/n^2, needs an odd step count
};

/// A known cash amount per share, with its ex-time in years from valuation.
struct CashDividend {
  double time;
  double amount;
};

/// Binomial-tree price of a vanilla option on a dividend-paying spot asset.
///
/// American exercise is checked at every node, so this is the reference model for
/// US equity options, where early exercise has real value (puts, and calls ahead
/// of dividends). Leisen-Reimer rounds `steps` up to the next odd number.
/// Inadmissible probabilities, saturated tails or unrepresentable nodes fall
/// back to BSM, floored by optimal deterministic exercise
/// for American contracts. This fallback is an approximation for positive vol.
/// Invalid inputs throw invalid_argument; unrepresentable fallback prices throw
/// overflow_error rather than returning NaN or a negative price.
/// Cash dividends strictly between valuation and expiry are escrowed at rate r;
/// the continuous yield applies to the remaining spot. Exercise adds back the
/// PV of cash still to come, including just before a dividend on a lattice date.
/// Amounts must be finite and nonnegative, times finite, and escrowed spot positive.
[[nodiscard]] double binomial_price(const BsmInputs& in, ExerciseStyle style, TreeMethod method,
                                    int steps, std::span<const CashDividend> dividends = {});

/// American minus European value on the SAME LR lattice: cancels European
/// discretisation error. With no cash dividends, exact zero when exercise cannot help (nonpositive q,
/// nonnegative r for calls; nonpositive r, nonnegative q for puts).
[[nodiscard]] double binomial_early_exercise_premium(
    const BsmInputs& in, int steps, std::span<const CashDividend> dividends = {});

}  // namespace openport::pricing

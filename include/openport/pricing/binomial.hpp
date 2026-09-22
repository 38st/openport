#pragma once

#include <cstdint>

#include "openport/pricing/black.hpp"

namespace openport::pricing {

enum class ExerciseStyle : std::uint8_t { European, American };

enum class TreeMethod : std::uint8_t {
  CoxRossRubinstein,  ///< u = exp(vol * sqrt(dt)); converges like 1/n with odd-even wobble
  LeisenReimer,       ///< Peizer-Pratt inversion; converges like 1/n^2, needs an odd step count
};

/// Binomial-tree price of a vanilla option on a dividend-paying spot asset.
///
/// American exercise is checked at every node, so this is the reference model for
/// US equity options, where early exercise has real value (puts, and calls ahead
/// of dividends). Leisen-Reimer rounds `steps` up to the next odd number.
[[nodiscard]] double binomial_price(const BsmInputs& in, ExerciseStyle style, TreeMethod method,
                                    int steps);

}  // namespace openport::pricing

#pragma once

#include <limits>
#include "openport/trading/types.hpp"

namespace openport::trading {
/// Gross option contracts held, summing each leg's absolute quantity. Pass only
/// option positions: shares never count. The projection supports ledger and view positions.
template <class Positions, class QuantityOf>
Quantity contracts_held(const Positions& positions, QuantityOf quantity_of) {
  Quantity total = 0;
  for (const auto& position : positions) {
    const auto q = quantity_of(position);
    if (q == std::numeric_limits<Quantity>::min())
      throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Contract count overflow");
    const auto size = q < 0 ? -q : q;
    if (__builtin_add_overflow(total, size, &total))
      throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Contract count overflow");
  }
  return total;
}
}  // namespace openport::trading

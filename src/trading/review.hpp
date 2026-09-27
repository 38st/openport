#pragma once

#include "state.hpp"

namespace openport::trading::detail {

/// Sample total round-trip P&L at the reducer's marking cadence, and on execution.
void update_reviews(State& state);

}  // namespace openport::trading::detail

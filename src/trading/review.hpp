#pragma once

#include <cstdlib>

#include "state.hpp"

namespace openport::trading::detail {

/// Sample total round-trip P&L at the reducer's marking cadence, and on execution.
/// Visits only the lifecycles whose reviews can still change (State::reviewing).
void update_reviews(State& state);
/// The same by rebuilding every lifecycle from every fill: what update_reviews
/// must match, review for review.
void update_reviews_rebuilt(State& state);
/// When set, update_reviews checks each of its results against
/// update_reviews_rebuilt and throws on a difference (OPENPORT_VERIFY_REVIEWS).
inline const bool verify_reviews = std::getenv("OPENPORT_VERIFY_REVIEWS") != nullptr;

}  // namespace openport::trading::detail

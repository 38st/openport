#pragma once

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "state.hpp"

namespace openport::trading::detail {

/// Sample total round-trip P&L at the reducer's marking cadence, and on execution.
/// Visits only the lifecycles whose reviews can still change (State::reviewing).
void update_reviews(State& state);
/// The same by rebuilding every lifecycle from every fill: what update_reviews
/// must match, review for review.
void update_reviews_rebuilt(State& state);
/// Recounts the unfinished reviews after a command changed them.
void recount_reviews(State& state);
/// Joins the trades of the listed open round trips into one (`together`), named
/// by the oldest, or takes each listed round trip out of its trade into one of its
/// own. The trades it changes restart their whole-trade reviews from now, when
/// they still hold more than one entry. Refuses unknown, closed or mixed
/// underlyings' round trips; changing nothing is no refusal.
[[nodiscard]] Decision regroup(State& state, const std::vector<std::uint64_t>& trades, bool together);
/// When set, update_reviews checks each of its results against
/// update_reviews_rebuilt and throws on a difference (OPENPORT_VERIFY_REVIEWS).
inline const bool verify_reviews = std::getenv("OPENPORT_VERIFY_REVIEWS") != nullptr;

}  // namespace openport::trading::detail

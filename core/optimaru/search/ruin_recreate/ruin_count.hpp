// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_RUIN_RECREATE_RUIN_COUNT_HPP
#define OPTIMARU_SEARCH_RUIN_RECREATE_RUIN_COUNT_HPP

#include <algorithm>

namespace optimaru {

// How many of its `n` candidates (n >= 1) a ruin removes: `drawn`, kept to at
// most n - 1 and the configured maximum, and to at least one. One also where
// n - 1 or the maximum is below one (a single entity or element): a ruin that
// removes nothing is no ruin. The stagnation kick, the destroy-repair move and
// the iterated local search all draw their count through here.
inline int kick_ruin_count(int drawn, int n, int max_ruin_count) {
    return std::clamp(drawn, 1, std::max(1, std::min(n - 1, max_ruin_count)));
}

// The same without a maximum of its own: at least one, at most n - 1. For
// n >= 2 exactly std::clamp(drawn, 1, n - 1).
inline int clamp_ruin_count(int drawn, int n) { return kick_ruin_count(drawn, n, n - 1); }

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_RUIN_RECREATE_RUIN_COUNT_HPP

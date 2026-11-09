// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_LOCAL_MOVABLE_ENTITY_HPP
#define OPTIMARU_LOCAL_MOVABLE_ENTITY_HPP

#include <random>

#include "optimaru/model/solution_traits.hpp"

namespace optimaru {

// Return a non-pinned entity: keep `e` if movable, otherwise resample (then
// scan) for one. Local search has checked that one exists, so the fallback
// to `e` is only a guard.
template <typename Solution>
int movable_entity(const Solution& sol, int e, std::uniform_int_distribution<int>& pick,
                  std::mt19937_64& rng) {
    using Traits = SolutionTraits<Solution>;
    if (!entity_is_pinned<Traits>(sol, e)) return e;
    for (int t = 0; t < 8; ++t) {
        const int c = pick(rng);
        if (!entity_is_pinned<Traits>(sol, c)) return c;
    }
    const int n = Traits::entity_count(sol);
    for (int c = 0; c < n; ++c)
        if (!entity_is_pinned<Traits>(sol, c)) return c;
    return e;
}

}  // namespace optimaru

#endif  // OPTIMARU_LOCAL_MOVABLE_ENTITY_HPP

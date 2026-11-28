// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_RELOCATE_HPP
#define OPTIMARU_SEARCH_MOVES_RELOCATE_HPP

#include <random>

#include "optimaru/solution/solution_traits.hpp"
#include "optimaru/search/moves/list_move.hpp"
#include "optimaru/search/moves/move_draw.hpp"
#include "optimaru/utils/random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Relocate (sequence shape)
// ---------------------------------------------------------------------------
// Move element e1 elsewhere: right after a nearby neighbour when nearby
// selection is configured, else to a uniform list and position.
// Draws the move without touching the solution; every coordinate is in the
// current solution, as ListMove defines it.
template <typename Solution>
SampledMove sample_relocate(const Solution& solution, MoveDraw<Solution>& draw,
                            std::mt19937_64& rng, int e1) {
    using Traits = SolutionTraits<Solution>;
    const int l1 = draw.index.list_of(e1), i1 = draw.index.position_of(e1);
    int l2, i2;
    const int nb = draw.nearby_pick(e1, rng);
    if (nb >= 0) {
        l2 = draw.index.list_of(nb);
        int q = draw.index.position_of(nb);
        if (l2 == l1 && q > i1) --q;  // nb shifts down once e1 is out
        i2 = q + 1;                   // insert right after the neighbour
    } else {
        l2 = random_index(rng, Traits::list_count(solution));
        const int size2 = Traits::list_size(solution, l2) - (l2 == l1 ? 1 : 0);
        i2 = random_index(rng, size2 + 1);
    }
    SampledMove move;
    move.list = ListMove::segment(l1, i1, 1, l2, i2, e1);
    move.entities[0] = e1;
    move.entity_count = 1;
    return move;
}

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_MOVES_RELOCATE_HPP

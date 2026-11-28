// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_ELEMENT_SWAP_HPP
#define OPTIMARU_SEARCH_MOVES_ELEMENT_SWAP_HPP

#include <random>

#include "optimaru/solution/solution_traits.hpp"
#include "optimaru/search/moves/list_move.hpp"
#include "optimaru/search/moves/move_draw.hpp"
#include "optimaru/utils/random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Element swap (sequence shape)
// ---------------------------------------------------------------------------
// Exchange element e1 with another element, possibly in another list; the
// partner is nearby-biased when nearby selection is configured.
// Draws the move without touching the solution; every coordinate is in the
// current solution, as ListMove defines it.
template <typename Solution>
SampledMove sample_element_swap(const Solution& solution, MoveDraw<Solution>& draw,
                                std::mt19937_64& rng, int e1) {
    using Traits = SolutionTraits<Solution>;
    const int l1 = draw.index.list_of(e1), i1 = draw.index.position_of(e1);
    int e2 = draw.nearby_pick(e1, rng);
    if (e2 < 0 || e2 == e1) e2 = random_other(Traits::element_count(solution), e1, rng);
    const int l2 = draw.index.list_of(e2), i2 = draw.index.position_of(e2);
    SampledMove move;
    move.list = ListMove::swap(l1, i1, l2, i2, e1, e2);
    move.entities[0] = e1;
    move.entities[1] = e2;
    move.entity_count = 2;
    return move;
}

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_MOVES_ELEMENT_SWAP_HPP

// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_TWO_OPT_HPP
#define OPTIMARU_SEARCH_MOVES_TWO_OPT_HPP

#include <algorithm>
#include <random>

#include "optimaru/solution/solution_traits.hpp"
#include "optimaru/search/moves/list_move.hpp"
#include "optimaru/search/moves/move_draw.hpp"
#include "optimaru/utils/random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// 2-opt (sequence shape)
// ---------------------------------------------------------------------------
// Reverse the segment between element e1 and another position of its list.
// Needs a list of at least two elements.
// Draws the move without touching the solution; every coordinate is in the
// current solution, as ListMove defines it.
template <typename Solution>
SampledMove sample_two_opt(const Solution& solution, MoveDraw<Solution>& draw, std::mt19937_64& rng,
                           int e1) {
    using Traits = SolutionTraits<Solution>;
    const int l1 = draw.index.list_of(e1), i1 = draw.index.position_of(e1);
    const int i2 = random_other(Traits::list_size(solution, l1), i1, rng);
    const int first = std::min(i1, i2), last = std::max(i1, i2);
    SampledMove move;
    move.list = ListMove::reverse(l1, first, last);
    move.entities[0] = Traits::element_at(solution, l1, first);
    move.entities[1] = Traits::element_at(solution, l1, last);
    move.entity_count = 2;
    return move;
}

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_MOVES_TWO_OPT_HPP

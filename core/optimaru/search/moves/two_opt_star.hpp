// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_TWO_OPT_STAR_HPP
#define OPTIMARU_SEARCH_MOVES_TWO_OPT_STAR_HPP

#include <random>

#include "optimaru/solution/solution_traits.hpp"
#include "optimaru/search/moves/list_move.hpp"
#include "optimaru/search/moves/move_draw.hpp"
#include "optimaru/utils/random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// 2-opt* (tail swap) (sequence shape)
// ---------------------------------------------------------------------------
// Exchange the tails of two lists, the classic inter-route move: e1's list is
// cut right before e1, the other list at a uniform position. Needs two lists.
// Draws the move without touching the solution; every coordinate is in the
// current solution, as ListMove defines it.
template <typename Solution>
SampledMove sample_two_opt_star(const Solution& solution, MoveDraw<Solution>& draw,
                                std::mt19937_64& rng, int e1) {
    using Traits = SolutionTraits<Solution>;
    const int l1 = draw.index.list_of(e1), i1 = draw.index.position_of(e1);
    const int l2 = random_other(Traits::list_count(solution), l1, rng);
    const int size2 = Traits::list_size(solution, l2);
    const int c2 = random_index(rng, size2 + 1);
    SampledMove move;
    move.entities[0] = e1;
    move.entities[1] = c2 < size2 ? Traits::element_at(solution, l2, c2) : e1;
    move.entity_count = 2;
    move.list = ListMove::tail_swap(l1, i1, l2, c2);
    return move;
}

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_MOVES_TWO_OPT_STAR_HPP

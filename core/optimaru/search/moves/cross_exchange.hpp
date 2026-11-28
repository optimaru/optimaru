// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_CROSS_EXCHANGE_HPP
#define OPTIMARU_SEARCH_MOVES_CROSS_EXCHANGE_HPP

#include <algorithm>
#include <random>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/search/moves/list_move.hpp"
#include "optimaru/search/moves/move_draw.hpp"
#include "optimaru/utils/random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// CROSS-exchange (sequence shape)
// ---------------------------------------------------------------------------
// Swap a run starting at e1 with a run in another list, the two runs sized
// independently up to MoveMix::cross_exchange_max_run. The partner list is
// chosen through a nearby neighbour when nearby selection is configured:
// exchanging with a spatially unrelated route is almost never an improvement.
// Needs two lists.
// Draws the move without touching the solution; every coordinate is in the
// current solution, as ListMove defines it.
template <typename Solution>
SampledMove sample_cross_exchange(const Solution& solution, MoveDraw<Solution>& draw,
                                  std::mt19937_64& rng, int e1) {
    using Traits = SolutionTraits<Solution>;
    const int l1 = draw.index.list_of(e1), i1 = draw.index.position_of(e1);
    const int size1 = Traits::list_size(solution, l1);
    int l2 = -1, i2 = 0;
    const int nb = draw.nearby_pick(e1, rng);
    if (nb >= 0 && draw.index.list_of(nb) >= 0 && draw.index.list_of(nb) != l1) {
        l2 = draw.index.list_of(nb);
        i2 = draw.index.position_of(nb);
    }
    if (l2 < 0) {
        l2 = random_other(Traits::list_count(solution), l1, rng);
        const int size2 = Traits::list_size(solution, l2);
        i2 = size2 == 0 ? 0 : random_index(rng, size2);
    }
    const int size2 = Traits::list_size(solution, l2);
    const int max_segment = std::max(1, draw.config.moves.cross_exchange_max_run);
    // One side may take zero elements, which degenerates to relocating
    // the other side's run: the cheap way to shorten a route.
    int len1 = random_index(rng, max_segment + 1);
    int len2 = random_index(rng, max_segment + 1);
    len1 = std::min(len1, size1 - i1);
    len2 = std::min(len2, size2 - i2);
    if (len1 <= 0 && len2 <= 0) {
        // e1 guarantees that this side has at least one available
        // element. Turn the zero/zero draw into a one-way segment
        // relocate instead of spending the step on a no-op.
        len1 = 1;
    }
    SampledMove move;
    move.entities[0] = e1;
    move.entities[1] = len2 > 0 ? Traits::element_at(solution, l2, i2) : e1;
    move.entity_count = 2;
    move.list = ListMove::exchange(l1, i1, len1, l2, i2, len2);
    return move;
}

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_MOVES_CROSS_EXCHANGE_HPP

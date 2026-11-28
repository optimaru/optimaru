// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_OR_OPT_HPP
#define OPTIMARU_SEARCH_MOVES_OR_OPT_HPP

#include <algorithm>
#include <random>

#include "optimaru/solution/solution_traits.hpp"
#include "optimaru/search/moves/list_move.hpp"
#include "optimaru/search/moves/move_draw.hpp"
#include "optimaru/utils/random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Or-opt (sequence shape)
// ---------------------------------------------------------------------------
// Relocate a short segment (2-3 consecutive elements around e1) intact,
// nearby-biased like relocate.
// Draws the move without touching the solution; every coordinate is in the
// current solution, as ListMove defines it.
template <typename Solution>
SampledMove sample_or_opt(const Solution& solution, MoveDraw<Solution>& draw, std::mt19937_64& rng,
                          int e1) {
    using Traits = SolutionTraits<Solution>;
    const int l1 = draw.index.list_of(e1), i1 = draw.index.position_of(e1);
    const int size1 = Traits::list_size(solution, l1);
    int seg_len = 2 + random_index(rng, 2);
    if (seg_len > size1) seg_len = size1;
    const int start = std::min(i1, size1 - seg_len);
    const int seg0 = Traits::element_at(solution, l1, start);
    int l2 = -1, i2 = 0;
    const int nb = draw.nearby_pick(seg0, rng);
    if (nb >= 0) {
        const int nb_list = draw.index.list_of(nb);
        int q = draw.index.position_of(nb);
        const bool in_segment = nb_list == l1 && q >= start && q < start + seg_len;
        if (!in_segment) {  // neighbor not part of the segment
            l2 = nb_list;
            if (l2 == l1 && q >= start + seg_len) q -= seg_len;
            i2 = q + 1;
        }
    }
    if (l2 < 0) {
        l2 = random_index(rng, Traits::list_count(solution));
        const int size2 = Traits::list_size(solution, l2) - (l2 == l1 ? seg_len : 0);
        i2 = random_index(rng, size2 + 1);
    }
    SampledMove move;
    move.list = ListMove::segment(l1, start, seg_len, l2, i2, seg0);
    move.entities[0] = seg0;
    move.entities[1] = Traits::element_at(solution, l1, start + seg_len - 1);
    move.entity_count = 2;
    return move;
}

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_MOVES_OR_OPT_HPP

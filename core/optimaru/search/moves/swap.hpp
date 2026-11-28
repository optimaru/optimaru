// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_SWAP_HPP
#define OPTIMARU_SEARCH_MOVES_SWAP_HPP

#include <random>

#include "optimaru/solution/editor.hpp"
#include "optimaru/solution/solution_traits.hpp"
#include "optimaru/search/moves/movable_entity.hpp"
#include "optimaru/search/moves/move_draw.hpp"
#include "optimaru/utils/random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Swap (assignment shape)
// ---------------------------------------------------------------------------
// Entity move.entities[0], which holds move.previous[0], and a partner
// exchange values. The partner is nearby-biased when nearby selection is
// configured, never pinned, and holds another value when any entity does.
// Applies the move.
//
// Expects move.entities[0] = e and move.previous[0] = e's value, filled in by
// MoveMixSampler before it chose this move.
template <typename Solution>
void apply_swap(Editor<Solution>& editor, MoveDraw<Solution>& draw, std::mt19937_64& rng,
                SampledMove& move) {
    using Traits = SolutionTraits<Solution>;
    Solution& solution = editor.solution();
    const int entity_count = Traits::entity_count(solution);
    const int e = move.entities[0];
    const int old_value = move.previous[0];
    int e2 = draw.nearby_pick(e, rng);
    if (e2 < 0) e2 = random_index(rng, entity_count);
    e2 = movable_entity(solution, e2, rng);  // no pinned swap partner
    for (int attempt = 0; attempt < 8 && (e2 == e || Traits::value_of(solution, e2) == old_value);
         ++attempt) {
        e2 = movable_entity(solution, random_index(rng, entity_count), rng);
    }
    if (e2 == e || Traits::value_of(solution, e2) == old_value) {
        for (int candidate = 0; candidate < entity_count; ++candidate) {
            if (candidate != e && !entity_is_pinned<Traits>(solution, candidate) &&
                Traits::value_of(solution, candidate) != old_value) {
                e2 = candidate;
                break;
            }
        }
    }
    const int old_value2 = Traits::value_of(solution, e2);
    editor.begin_batch();
    editor.assign(e, old_value2);
    editor.assign(e2, old_value);
    editor.end_batch();
    move.entities[1] = e2;
    move.previous[1] = old_value2;
    move.entity_count = 2;
}

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_MOVES_SWAP_HPP

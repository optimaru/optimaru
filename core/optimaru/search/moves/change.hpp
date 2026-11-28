// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_CHANGE_HPP
#define OPTIMARU_SEARCH_MOVES_CHANGE_HPP

#include <random>

#include "optimaru/model/editor.hpp"
#include "optimaru/model/solution_traits.hpp"
#include "optimaru/search/moves/move_draw.hpp"
#include "optimaru/settings.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Change (assignment shape)
// ---------------------------------------------------------------------------
// Give entity move.entities[0], which holds move.previous[0], another value:
// the use case's value hint when it has one, else a uniform value, never the
// current one when there is a choice. Applies the move.
template <typename Solution>
void apply_change(Editor<Solution>& editor, MoveDraw<Solution>& draw,
                  std::uniform_int_distribution<int>& value_pick, std::mt19937_64& rng,
                  SampledMove& move) {
    using Traits = SolutionTraits<Solution>;
    const SelectionSettings& select = draw.config.selection;
    const int value_count = Traits::value_count(editor.solution());
    const int e = move.entities[0];
    const int old_value = move.previous[0];
    int v = select.value_hint ? select.value_hint(rng, e) : -1;
    if (v < 0 || (value_count > 1 && v == old_value)) {
        if (value_count > 1 && old_value >= 0 && old_value < value_count) {
            v = random_other(value_count, old_value, rng);
        } else {
            v = value_pick(rng);
        }
    }
    editor.assign(e, v);
    move.entity_count = 1;
}

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_MOVES_CHANGE_HPP

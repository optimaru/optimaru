// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_RUIN_REMOVAL_GAIN_HPP
#define OPTIMARU_SEARCH_RUIN_REMOVAL_GAIN_HPP

#include "optimaru/solution_traits.hpp"
#include "optimaru/score.hpp"
#include "optimaru/search/operators.hpp"

namespace optimaru {

// Score after temporarily removing/unassigning one entity (higher = its
// removal helps more = "worse" placed). Shared by the ruin strategies that
// rank candidates; the solution is restored before returning.
template <typename Solution>
Score removal_gain(OperatorContext<Solution>& ctx, int e) {
    using Traits = SolutionTraits<Solution>;
    Editor<Solution>& editor = ctx.editor;
    if constexpr (Traits::shape == Shape::Assignment) {
        int old = Traits::value_of(editor.solution(), e);
        editor.assign(e, -1);
        Score gain = editor.score();
        editor.assign(e, old);
        return gain;
    } else {
        int l = ctx.element_list[e], p = ctx.element_position[e];
        editor.remove(l, p);
        Score gain = editor.score();
        editor.insert(l, p, e);  // positions restored: index unchanged
        return gain;
    }
}

// How many entities (assignment) or elements (sequence) the ruin may draw from.
template <typename Solution>
int ruin_candidate_count(OperatorContext<Solution>& ctx) {
    using Traits = SolutionTraits<Solution>;
    if constexpr (Traits::shape == Shape::Assignment) {
        return Traits::entity_count(ctx.editor.solution());
    } else {
        return Traits::element_count(ctx.editor.solution());
    }
}

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_RUIN_REMOVAL_GAIN_HPP

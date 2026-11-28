// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_CONSTRUCT_FIRST_FIT_HPP
#define OPTIMARU_CONSTRUCT_FIRST_FIT_HPP

#include <vector>

#include "optimaru/construct/initialization_order.hpp"
#include "optimaru/solution/editor.hpp"
#include "optimaru/solution/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/settings.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// First fit (assignment shape)
// ---------------------------------------------------------------------------
// Give every entity without a value, in initialization order, the value with
// the best score at that point. With a difficulty hook this is first fit
// decreasing.

// Assign entity e the value with the best score (the first on ties). The
// ruin-and-recreate repair reuses it.
template <typename Solution>
void place_best_value(Editor<Solution>& editor, int e, int value_count) {
    Score best_score;
    bool best_initialized = false;
    int best_value = 0;
    for (int v = 0; v < value_count; ++v) {
        editor.assign(e, v);
        Score candidate = editor.score();
        if (!best_initialized || candidate > best_score) {
            best_score = candidate;
            best_initialized = true;
            best_value = v;
        }
    }
    editor.assign(e, best_value);
}

template <typename Solution>
void first_fit(Editor<Solution>& editor, const ConstructionSettings& config) {
    using Traits = SolutionTraits<Solution>;
    static_assert(Traits::shape == Shape::Assignment, "first_fit builds an assignment");
    Solution& solution = editor.solution();
    const int entity_count = Traits::entity_count(solution);
    const int value_count = Traits::value_count(solution);
    if (value_count == 0) return;
    std::vector<int> uninitialized;
    for (int e = 0; e < entity_count; ++e) {
        if (Traits::value_of(solution, e) < 0) uninitialized.push_back(e);
    }
    for (int e : optimaru::initialization_order(uninitialized, config.difficulty)) {
        optimaru::place_best_value(editor, e, value_count);
    }
}

}  // namespace optimaru

#endif  // OPTIMARU_CONSTRUCT_FIRST_FIT_HPP

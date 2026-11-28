// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_CONSTRUCT_CHEAPEST_INSERTION_HPP
#define OPTIMARU_CONSTRUCT_CHEAPEST_INSERTION_HPP

#include <vector>

#include "optimaru/construct/initialization_order.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/settings.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Cheapest insertion (sequence shape)
// ---------------------------------------------------------------------------
// Insert every element in no list, in initialization order, at the (list,
// position) with the best score at that point.

// Insert element e at the best position over all (list, position) pairs (the
// first on ties). Does not touch the local search's element index.
template <typename Solution>
void place_cheapest_insertion_full_scan(Editor<Solution>& editor, int e) {
    using Traits = SolutionTraits<Solution>;
    Solution& solution = editor.solution();
    const int list_count = Traits::list_count(solution);
    Score best_score;
    bool best_initialized = false;
    int best_list = 0, best_pos = 0;
    for (int l = 0; l < list_count; ++l) {
        const int size = Traits::list_size(solution, l);
        for (int p = 0; p <= size; ++p) {
            editor.insert(l, p, e);
            Score candidate = editor.score();
            editor.remove(l, p);
            if (!best_initialized || candidate > best_score) {
                best_score = candidate;
                best_initialized = true;
                best_list = l;
                best_pos = p;
            }
        }
    }
    editor.insert(best_list, best_pos, e);
}

template <typename Solution>
void cheapest_insertion(Editor<Solution>& editor, const ConstructionSettings& config) {
    using Traits = SolutionTraits<Solution>;
    static_assert(Traits::shape == Shape::Sequence, "cheapest_insertion builds lists");
    Solution& solution = editor.solution();
    const int list_count = Traits::list_count(solution);
    const int element_count = Traits::element_count(solution);
    if (list_count == 0) return;
    std::vector<char> assigned(element_count, 0);
    for (int l = 0; l < list_count; ++l) {
        for (int p = 0; p < Traits::list_size(solution, l); ++p) {
            assigned[Traits::element_at(solution, l, p)] = 1;
        }
    }
    std::vector<int> unassigned;
    for (int e = 0; e < element_count; ++e) {
        if (!assigned[e]) unassigned.push_back(e);
    }
    for (int e : optimaru::initialization_order(unassigned, config.difficulty)) {
        optimaru::place_cheapest_insertion_full_scan(editor, e);
    }
}

}  // namespace optimaru

#endif  // OPTIMARU_CONSTRUCT_CHEAPEST_INSERTION_HPP

// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_CONSTRUCT_CONSTRUCTION_HPP
#define OPTIMARU_CONSTRUCT_CONSTRUCTION_HPP

#include <algorithm>
#include <functional>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/settings.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
// Greedy construction (first fit, or first fit decreasing with a difficulty
// hook): give every unassigned entity the placement with the best score at
// that point. Assignment shape: its best value; sequence shape: cheapest
// insertion (best list and position). The ruin-and-recreate repair reuses
// the placement helpers.
template <typename Solution>
struct Construction {
    using Traits = SolutionTraits<Solution>;

    static void run(Editor<Solution>& editor, const ConstructionSettings& config) {
        Solution& solution = editor.solution();
        if constexpr (Traits::shape == Shape::Assignment) {
            const int entity_count = Traits::entity_count(solution);
            const int value_count = Traits::value_count(solution);
            if (value_count == 0) return;
            std::vector<int> uninitialized;
            for (int e = 0; e < entity_count; ++e) {
                if (Traits::value_of(solution, e) < 0) uninitialized.push_back(e);
            }
            for (int e : initialization_order(uninitialized, config.difficulty)) {
                place_best_value(editor, e, value_count);
            }
        } else {
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
            for (int e : initialization_order(unassigned, config.difficulty)) {
                place_cheapest_insertion_full_scan(editor, e);
            }
        }
    }

    // Initialization order: decreasing difficulty if a hook is set (FFD),
    // otherwise index order (First Fit).
    static std::vector<int> initialization_order(
            const std::vector<int>& candidates,
            const std::function<double(int)>& difficulty) {
        std::vector<int> order = candidates;
        if (difficulty) {
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                return difficulty(a) > difficulty(b);
            });
        }
        return order;
    }

    // assignment: assign entity e to its best value.
    static void place_best_value(Editor<Solution>& editor, int e, int value_count) {
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

    // sequence: insert element e at the best position over ALL (list, position)
    // pairs. Does not touch the local-search element index.
    static void place_cheapest_insertion_full_scan(Editor<Solution>& editor, int e) {
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
};

}  // namespace optimaru

#endif  // OPTIMARU_CONSTRUCT_CONSTRUCTION_HPP

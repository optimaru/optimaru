// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_CONSTRUCT_CONSTRUCTION_HPP
#define OPTIMARU_CONSTRUCT_CONSTRUCTION_HPP

#include <functional>

#include "optimaru/construct/cheapest_insertion.hpp"
#include "optimaru/construct/first_fit.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/model/solution_traits.hpp"
#include "optimaru/settings.hpp"

namespace optimaru {

// Whether the solution comes in with nothing placed that the search may move:
// every entity pinned or without a value (assignment shape), every list empty
// (sequence shape). The construction then builds the whole start; otherwise
// the solve starts from what the caller placed (a warm start), completed by
// the construction where something is missing. A use case asks before it
// sets what only suits a start from the construction
// (AcceptanceSettings::history_growth).
template <typename Solution>
bool nothing_placed(const Solution& solution) {
    using Traits = SolutionTraits<Solution>;
    if constexpr (Traits::shape == Shape::Assignment) {
        const int entity_count = Traits::entity_count(solution);
        for (int e = 0; e < entity_count; ++e) {
            if (Traits::value_of(solution, e) >= 0 && !entity_is_pinned<Traits>(solution, e))
                return false;
        }
    } else {
        const int list_count = Traits::list_count(solution);
        for (int l = 0; l < list_count; ++l) {
            if (Traits::list_size(solution, l) > 0) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
// The start of a solve: what places everything the caller left unplaced. The
// built-in heuristics, one file each:
//   assignment shape: first_fit.hpp (first fit, or first fit decreasing with
//                     ConstructionSettings::difficulty);
//   sequence shape:   cheapest_insertion.hpp.
// A use case with a construction of its own registers it on the solver
// (Solver::set_construction) in their place. A new built-in heuristic is a
// file here and a branch in run() below.
template <typename Solution>
using ConstructionFn = std::function<void(Editor<Solution>&, const ConstructionSettings&)>;

template <typename Solution>
struct Construction {
    using Traits = SolutionTraits<Solution>;

    // The built-in heuristic of the solution's shape.
    static void run(Editor<Solution>& editor, const ConstructionSettings& config) {
        if constexpr (Traits::shape == Shape::Assignment) {
            first_fit(editor, config);
        } else {
            cheapest_insertion(editor, config);
        }
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_CONSTRUCT_CONSTRUCTION_HPP

// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_EXPLAIN_HPP
#define OPTIMARU_EXPLAIN_HPP

#include "optimaru/score/breakdown.hpp"
#include "optimaru/score/weighted_objective.hpp"
#include "optimaru/score/objective.hpp"

namespace optimaru {

// Scoring outside a solve, for a solved or hand-built solution. Both rebuild
// the objective from the solution first, so they never trust incremental
// state left over from earlier edits.

// The score with its per-constraint split. The split needs the constraints
// registered as named components of a WeightedObjective.
template <typename Solution>
ScoreBreakdown explain_score(Solution& solution, WeightedObjective<Solution>& objective) {
    objective.rebuild(solution);
    return objective.breakdown();
}

// Any other objective: the total only, as a single entry named "score".
template <typename Solution>
ScoreBreakdown explain_score(Solution& solution, Objective<Solution>& objective) {
    objective.rebuild(solution);
    ScoreBreakdown breakdown;
    breakdown.score = objective.score();
    breakdown.constraints.push_back({"score", breakdown.score});
    return breakdown;
}

// The score of `solution`, recomputed from scratch.
template <typename Solution>
Score rescore(Solution& solution, Objective<Solution>& objective) {
    objective.rebuild(solution);
    return objective.score();
}

}  // namespace optimaru

#endif  // OPTIMARU_EXPLAIN_HPP

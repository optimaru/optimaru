#ifndef QAP_SCORE_CALCULATOR_HPP
#define QAP_SCORE_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "qap/domain.hpp"

namespace qap
{

// The quadratic-assignment cost function as one monolithic full recompute —
// the reference the incremental calculator is checked against (in the tests
// and under FULL_ASSERT), and the score the default assembly of
// qap/constraints.hpp reproduces bit-for-bit.
//
// Constraints (HardSoftScore), at the components' kDefaultWeight (all 1):
//   hard 1. pinnedAssignmentsCost      fixed locations not holding their
//                                      facility (an empty one included)
//   hard 2. forbiddenAssignmentsCost   facilities standing at a location a
//                                      (deduplicated) forbidden row names
//   hard 3. locationCapacityCost       demand above each capacitated
//                                      location's capacity
//   soft 4. assignmentCost             summed direct facility-location cost
//   soft 5. flowDistanceCost           summed flows[a][b] * distances[loc a][loc b]
// Unassigned facilities (location == -1) contribute nothing (only a fixed
// location they should stand at counts).
class ScoreCalculator
{
public:
    // Raw totals of the five constraints (unweighted, unshaped), in catalog
    // order.
    struct RawCosts
    {
        std::int64_t pinnedAssignmentsCost = 0;
        std::int64_t forbiddenAssignmentsCost = 0;
        std::int64_t locationCapacityCost = 0;
        std::int64_t assignmentCost = 0;
        std::int64_t flowDistanceCost = 0;
    };

    // Full recompute at the default weights; also stores problem.score.
    static Score calculateScore(QuadraticAssignmentProblem &problem);

    // The raw totals the score is made of (prepares the problem).
    static RawCosts rawCosts(QuadraticAssignmentProblem &problem);

    // Validates the facts and builds the flat caches (flow / distance /
    // direct matrices, demand and capacity per index, the fixed and forbidden
    // lookups, the scoped location lists). Idempotent (guarded by
    // QuadraticAssignmentProblem::prepared). Throws std::invalid_argument on
    // an inconsistent problem: facility / location counts that differ,
    // non-square or mis-sized matrices, an out-of-range index, a location
    // fixed twice, a negative demand or capacity, a facility location out of
    // range; std::overflow_error when the objective magnitudes could
    // overflow int64.
    static void prepare(QuadraticAssignmentProblem &problem);
};

}  // namespace qap

#endif  // QAP_SCORE_CALCULATOR_HPP

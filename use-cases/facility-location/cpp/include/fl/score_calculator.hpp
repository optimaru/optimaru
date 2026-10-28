#ifndef FL_SCORE_CALCULATOR_HPP
#define FL_SCORE_CALCULATOR_HPP

#include "fl/domain.hpp"

namespace fl
{

// Port of the Timefold facility-location FacilityLocationConstraintProvider.
//
// calculateScore does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under FULL_ASSERT).
//
// Constraints (HardSoftScore):
//   hard 1. facilityCapacity       -(usedDemand - capacity) per used facility
//                                  whose assigned demand exceeds its capacity
//                                  (groupBy(facility, sum(demand)) in Java, so
//                                  only facilities with >= 1 consumer count)
//   soft 2. facilitySetupCost      -setupCostWeight * setupCost per facility
//                                  serving at least one consumer (weight 2)
//   soft 3. distanceFromFacility   -distanceFromFacilityWeight * meters per
//                                  assigned consumer (provider default 5,
//                                  overridden to 6 by the solution's
//                                  ConstraintWeightOverrides — see domain.hpp)
// Unassigned consumers (facility == -1) contribute nothing: the construction
// heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    static Score calculateScore(FacilityLocationProblem &problem);

    // Precomputes the consumer->facility distance matrix so scoring reads
    // are O(1) integer lookups. Idempotent (guarded by
    // FacilityLocationProblem::prepared). Must run before scoring;
    // calculateScore/solve call it.
    static void prepare(FacilityLocationProblem &problem);
};

}  // namespace fl

#endif  // FL_SCORE_CALCULATOR_HPP

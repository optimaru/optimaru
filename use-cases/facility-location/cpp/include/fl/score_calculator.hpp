#ifndef FL_SCORE_CALCULATOR_HPP
#define FL_SCORE_CALCULATOR_HPP

#include "fl/domain.hpp"

namespace fl
{

// Port of the reference facility-location FacilityLocationConstraintProvider.
//
// calculateScore does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under FULL_ASSERT).
//
// Constraints (HardSoftScore), the default assembly of fl/constraints.hpp at
// the components' kDefaultWeight:
//   hard 1. facilityCapacityCost       -(usedDemand - capacity) per used
//                                      facility whose assigned demand exceeds
//                                      its capacity (groupBy(facility,
//                                      sum(demand)) in Java, so only
//                                      facilities with >= 1 consumer count)
//   soft 2. facilitySetupCost          -2 * setupCost per facility serving at
//                                      least one consumer
//   soft 3. distanceFromFacilityCost   -6 * meters per assigned consumer
//                                      (provider default 5, overridden to 6
//                                      by the solution's
//                                      ConstraintWeightOverrides)
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

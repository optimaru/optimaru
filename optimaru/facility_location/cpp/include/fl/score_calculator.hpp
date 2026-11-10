#ifndef FL_SCORE_CALCULATOR_HPP
#define FL_SCORE_CALCULATOR_HPP

#include "fl/domain.hpp"

namespace fl
{

// The facility-location score as one monolithic function.
//
// score does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under ScoreChecks::Full).
//
// Constraints (Score), the default assembly of fl/constraints.hpp at
// the components' kDefaultWeight:
//   hard 1. facilityCapacityCost       -(usedDemand - capacity) per used
//                                      facility whose assigned demand exceeds
//                                      its capacity (only facilities with
//                                      >= 1 consumer count)
//   soft 2. facilitySetupCost          -2 * setupCost per facility serving at
//                                      least one consumer
//   soft 3. distanceFromFacilityCost   -6 * meters per assigned consumer
// Unassigned consumers (facility == -1) contribute nothing: the construction
// heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    static Score score(FacilityLocationProblem &problem);

    // Precomputes the consumer->facility distance matrix so scoring reads
    // are O(1) integer lookups. Idempotent (guarded by
    // FacilityLocationProblem::prepared). Must run before scoring;
    // score/solve call it.
    static void prepare(FacilityLocationProblem &problem);
};

}  // namespace fl

#endif  // FL_SCORE_CALCULATOR_HPP

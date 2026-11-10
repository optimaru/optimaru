#ifndef VRP_SCORE_CALCULATOR_HPP
#define VRP_SCORE_CALCULATOR_HPP

#include "vrp/domain.hpp"

namespace vrp {

// The four core constraints:
//
//   HARD   vehicleCapacityCost    route demand over capacity
//   HARD   maxEndTimeCost         time-window lateness, in whole minutes
//   MEDIUM unassignedVisitsCost   penalty minutes of the unrouted visits
//                                 (folded into soft x 10^9 by toEngineScore)
//   SOFT   travelTimeCost         total driving seconds
//
// each at its component's kDefaultWeight (vrp/constraints/*.hpp). The
// route-policy and opt-in components of the default assembly cost 0 on a plan
// that sets none of their parameters, which is when the assembly and this
// calculator agree bit for bit.
//
// This recomputes the full score from the current vehicle routes; the
// search itself uses the incremental calculator.
class ScoreCalculator {
public:
    // Recomputes every visit's derived fields (vehicleIndex, arrivalTime)
    // from the current vehicle.visitIndices lists. Must be called before
    // score whenever routes change.
    static void updateShadowVariables(VehicleRoutePlan& plan);

    // Recomputes the derived fields, then the Score, storing it in
    // plan.score and returning it.
    static Score score(VehicleRoutePlan& plan);

private:
    // Ceil of a positive second-count to whole minutes, matching
    // Visit.roundDurationToNextOrEqualMinutes.
    static std::int64_t ceilToMinutes(Seconds seconds);
};

}  // namespace vrp

#endif  // VRP_SCORE_CALCULATOR_HPP

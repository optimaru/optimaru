#ifndef VRP_SCORE_CALCULATOR_HPP
#define VRP_SCORE_CALCULATOR_HPP

#include "vrp/domain.hpp"

namespace vrp {

// Port of org.acme.vehiclerouting.solver.VehicleRoutingConstraintProvider.
//
// Unlike an incremental constraint-stream engine, this recomputes the
// full score from the current vehicle routes. For the quickstart's problem size
// (tens of visits) a full O(n) recompute per move is more than fast enough.
class ScoreCalculator {
public:
    // Recomputes every visit's shadow variables (vehicleIndex, arrivalTime)
    // from the current vehicle.visitIndices lists. Must be called before
    // calculateScore whenever routes change.
    static void updateShadowVariables(VehicleRoutePlan& plan);

    // Recomputes shadow variables, then the HardMediumSoftScore, storing it in
    // plan.score and returning it.
    static Score calculateScore(VehicleRoutePlan& plan);

private:
    // Ceil of a positive second-count to whole minutes, matching
    // Visit.roundDurationToNextOrEqualMinutes.
    static std::int64_t ceilToMinutes(Seconds seconds);
};

}  // namespace vrp

#endif  // VRP_SCORE_CALCULATOR_HPP

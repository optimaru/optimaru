#ifndef PDP_SCORE_CALCULATOR_HPP
#define PDP_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "pdp/domain.hpp"

namespace pdp
{

// ---------------------------------------------------------------------------
// The PDPTW constraints (the full-recompute reference implementation)
// ---------------------------------------------------------------------------
// Applies the pickup-and-delivery constraint set:
//
//   HARD  pairedRequest        both nodes of a request are routed, or neither
//   HARD  sameVehicle          a request's pickup and dropoff share a vehicle
//   HARD  pickupBeforeDropoff  and the pickup comes first in that route
//   HARD  vehicleCapacity      the running load never exceeds the capacity
//   HARD  serviceFinishedAfterMaxEndTime   time-window lateness, in minutes
//   MEDIUM unassignedRequests  service minutes of the requests left unrouted
//   SOFT   minimizeTravelTime  total driving seconds
//
// The first three are what make this a PDP rather than a VRP, and they are
// where a naive element-granular local search burns its budget — hence the
// pair-aware operators in solver.hpp.
class ScoreCalculator
{
public:
    // Penalty weight (in the same whole-minute units as lateness) for each
    // violated coupling rule. Any nonzero hard total already means infeasible;
    // these values only shape the gradient *within* the infeasible region, so
    // they are set well above a typical lateness so the search fixes broken
    // pairs before it polishes time windows.
    static constexpr std::int64_t kCouplingPenalty = 1000;
    // Above any total the other hard rules can reach, so no trade between hard
    // violations pays for disturbing a pinned stop.
    static constexpr std::int64_t kPinnedPenalty = 1'000'000'000'000;

    // Recomputes every node's shadow variables (vehicleIndex, arrivalTime)
    // from the current vehicle routes.
    static void updateShadowVariables(PdpRoutePlan &plan);

    // Recomputes shadow variables, then the HardMediumSoftScore, storing it in
    // plan.score and returning it.
    static Score calculateScore(PdpRoutePlan &plan);

    // Ceil of a positive second-count to whole minutes.
    static std::int64_t ceilToMinutes(Seconds seconds)
    {
        return seconds <= 0 ? 0 : (seconds + 59) / 60;
    }
};

}  // namespace pdp

#endif  // PDP_SCORE_CALCULATOR_HPP

#ifndef PDP_SCORE_CALCULATOR_HPP
#define PDP_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "pdp/domain.hpp"

namespace pdp
{

// ---------------------------------------------------------------------------
// The PDPTW constraints (the full-recompute reference implementation)
// ---------------------------------------------------------------------------
// Applies the seven core pickup-and-delivery constraints, at the components'
// kDefaultWeight (pdp/constraints/*.hpp):
//
//   HARD   pairedRequestCost        both nodes of a request are routed, or
//                                   neither (x1000)
//   HARD   sameVehicleCost          a request's pickup and dropoff share a
//                                   vehicle (x1000)
//   HARD   pickupBeforeDropoffCost  and the pickup comes first in that route
//                                   (x1000)
//   HARD   vehicleCapacityCost      the running load never exceeds the capacity
//   HARD   maxEndTimeCost           time-window lateness, in minutes
//   MEDIUM unassignedRequestsCost   service minutes of the requests left
//                                   unrouted (folded into soft x 10^9 by
//                                   toEngineScore)
//   SOFT   travelTimeCost           total driving seconds
//
// The route-policy, pinning, binding and opt-in components of the default
// assembly cost 0 on a plan that sets none of their parameters, which is when
// the assembly and this calculator agree bit for bit.
//
// The first three are what make this a PDP rather than a VRP, and they are
// where a naive element-granular local search burns its budget — hence the
// pair-aware operators in solver.hpp.
class ScoreCalculator
{
public:
    // Penalty weight (in the same whole-minute units as lateness) for each
    // violated coupling rule — the kDefaultWeight of pairedRequestCost,
    // sameVehicleCost, pickupBeforeDropoffCost and requiredVehicleCost. Any
    // nonzero hard total already means infeasible; these values only shape
    // the gradient *within* the infeasible region, so they are set well above
    // a typical lateness so the search fixes broken pairs before it polishes
    // time windows.
    static constexpr std::int64_t kCouplingPenalty = 1000;
    // Far above what the other hard rules total on any real input (lateness
    // has no ceiling, but it would take 1e12 minutes of it), so no trade between
    // hard violations pays for disturbing a pinned stop. pinnedStopsCost's
    // kDefaultWeight.
    static constexpr std::int64_t kPinnedPenalty = 1'000'000'000'000;

    // Recomputes every node's shadow variables (vehicleIndex, arrivalTime)
    // from the current vehicle routes.
    static void updateShadowVariables(PdpRoutePlan &plan);

    // Recomputes shadow variables, then the HardMediumSoftScore, storing it in
    // plan.score and returning it.
    static Score score(PdpRoutePlan &plan);

    // Ceil of a positive second-count to whole minutes.
    static std::int64_t ceilToMinutes(Seconds seconds)
    {
        return seconds <= 0 ? 0 : (seconds + 59) / 60;
    }
};

}  // namespace pdp

#endif  // PDP_SCORE_CALCULATOR_HPP

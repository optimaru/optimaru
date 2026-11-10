#ifndef PDP_CONSTRAINTS_PICKUP_BEFORE_DROPOFF_COST_HPP
#define PDP_CONSTRAINTS_PICKUP_BEFORE_DROPOFF_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard, weight 1000 (ScoreCalculator::kCouplingPenalty):
// pickupBeforeDropoffCost — a request whose dropoff is scheduled before its
// pickup on the same route. Only checked once both nodes share a vehicle;
// sameVehicleCost prices the split case. Raw cost (global) = the number of
// such reversed requests, so 0 = every pickup precedes its dropoff.
// Structural on the CP-SAT path only (its plugin refuses level='soft').
struct PickupBeforeDropoffCost final : RequestCouplingConstraint
{
    static constexpr double kDefaultWeight
        = static_cast<double>(ScoreCalculator::kCouplingPenalty);
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit PickupBeforeDropoffCost(double weight = kDefaultWeight)
        : RequestCouplingConstraint(
              "pickupBeforeDropoffCost", weight, kDefaultLevel)
    {
    }
    std::int64_t requestCost(const PdpRoutePlan & /*plan*/,
                             const Delivery & /*delivery*/,
                             int pickupUnit,
                             int pickupPosition,
                             int dropoffUnit,
                             int dropoffPosition) const override
    {
        if (pickupUnit < 0 || dropoffUnit < 0 || pickupUnit != dropoffUnit)
            return 0;
        return pickupPosition > dropoffPosition ? 1 : 0;
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_PICKUP_BEFORE_DROPOFF_COST_HPP

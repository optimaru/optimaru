#ifndef PDP_CONSTRAINTS_PICKUP_BEFORE_DROPOFF_HPP
#define PDP_CONSTRAINTS_PICKUP_BEFORE_DROPOFF_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard (default, weight kCouplingPenalty): pickupBeforeDropoff — a request
// whose dropoff is scheduled before its pickup on the same route. Only checked
// once both nodes share a vehicle; sameVehicle prices the split case.
struct PickupBeforeDropoff final : RequestCouplingConstraint
{
    explicit PickupBeforeDropoff(
        double weight = static_cast<double>(ScoreCalculator::kCouplingPenalty))
        : RequestCouplingConstraint(
              "pickupBeforeDropoff", weight, /*hard=*/true)
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

#endif  // PDP_CONSTRAINTS_PICKUP_BEFORE_DROPOFF_HPP

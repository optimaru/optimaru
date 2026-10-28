#ifndef PDP_CONSTRAINTS_SAME_VEHICLE_HPP
#define PDP_CONSTRAINTS_SAME_VEHICLE_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard (default, weight kCouplingPenalty): sameVehicle — a request whose
// pickup and dropoff are routed on two DIFFERENT vehicles. Nobody hands the
// goods over mid-route, so the two nodes have to travel together.
//
// Silent when the request is half-routed or fully unrouted: pairedRequest and
// unassignedRequests price those cases, so each rule stays one term.
struct SameVehicle final : RequestCouplingConstraint
{
    explicit SameVehicle(
        double weight = static_cast<double>(ScoreCalculator::kCouplingPenalty))
        : RequestCouplingConstraint("sameVehicle", weight, /*hard=*/true)
    {
    }
    std::int64_t requestCost(const PdpRoutePlan & /*plan*/,
                             const Delivery & /*delivery*/,
                             int pickupUnit,
                             int /*pickupPosition*/,
                             int dropoffUnit,
                             int /*dropoffPosition*/) const override
    {
        if (pickupUnit < 0 || dropoffUnit < 0)
            return 0;
        return pickupUnit != dropoffUnit ? 1 : 0;
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_SAME_VEHICLE_HPP

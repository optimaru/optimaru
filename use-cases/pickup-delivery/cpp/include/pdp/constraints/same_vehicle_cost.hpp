#ifndef PDP_CONSTRAINTS_SAME_VEHICLE_COST_HPP
#define PDP_CONSTRAINTS_SAME_VEHICLE_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard, weight 1000 (ScoreCalculator::kCouplingPenalty): sameVehicleCost — a
// request whose pickup and dropoff are routed on two DIFFERENT vehicles.
// Nobody hands the goods over mid-route, so the two nodes have to travel
// together. Raw cost (global) = the number of such split requests, so 0 =
// every fully routed request rides one vehicle.
//
// Silent when the request is half-routed or fully unrouted: pairedRequestCost
// and unassignedRequestsCost price those cases, so each rule stays one term.
// Structural on the CP-SAT path only (its plugin refuses level='soft').
struct SameVehicleCost final : RequestCouplingConstraint
{
    static constexpr double kDefaultWeight
        = static_cast<double>(ScoreCalculator::kCouplingPenalty);
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit SameVehicleCost(double weight = kDefaultWeight)
        : RequestCouplingConstraint("sameVehicleCost", weight, kDefaultLevel)
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

#endif  // PDP_CONSTRAINTS_SAME_VEHICLE_COST_HPP

#ifndef PDP_CONSTRAINTS_UNASSIGNED_REQUESTS_COST_HPP
#define PDP_CONSTRAINTS_UNASSIGNED_REQUESTS_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Medium, weight 1 (medium units): unassignedRequestsCost — the service
// minutes of every request with BOTH nodes unrouted, so dropping a long job
// hurts more than dropping a short one. Raw cost (global) = the sum over
// fully unrouted requests of floor((pickup + dropoff service seconds) / 60);
// 0 = no request is left out. A half-routed request is not counted here:
// pairedRequestCost prices that on the hard level instead.
//
// The domain folds the medium level into the engine's soft level as
// `medium * kMediumUnit` (kMediumFold in domain.hpp); the fold lives in the
// calculator, not in this weight. hard() gives 1 hard per minute, soft() 1
// soft per minute.
struct UnassignedRequestsCost final : RequestCouplingConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Medium;

    explicit UnassignedRequestsCost(double weight = kDefaultWeight)
        : RequestCouplingConstraint(
              "unassignedRequestsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t requestCost(const PdpRoutePlan &plan,
                             const Delivery &delivery,
                             int pickupUnit,
                             int /*pickupPosition*/,
                             int dropoffUnit,
                             int /*dropoffPosition*/) const override
    {
        if (pickupUnit >= 0 || dropoffUnit >= 0)
            return 0;
        const Node &pickup
            = plan.nodes[static_cast<std::size_t>(delivery.pickupNodeIndex)];
        const Node &dropoff
            = plan.nodes[static_cast<std::size_t>(delivery.dropoffNodeIndex)];
        return (pickup.serviceDuration + dropoff.serviceDuration) / 60;
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_UNASSIGNED_REQUESTS_COST_HPP

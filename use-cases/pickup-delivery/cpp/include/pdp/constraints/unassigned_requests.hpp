#ifndef PDP_CONSTRAINTS_UNASSIGNED_REQUESTS_HPP
#define PDP_CONSTRAINTS_UNASSIGNED_REQUESTS_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Medium (default, weight 1 medium level): unassignedRequests — the service
// minutes of every request with BOTH nodes unrouted, so dropping a long job
// hurts more than dropping a short one. A half-routed request is not counted
// here: pairedRequest prices that on the hard level instead.
//
// The medium level is folded into the engine's soft level as
// `medium * kMediumUnit` (see domain.hpp), which is what the default weight
// carries; the component's own cost stays plain minutes.
struct UnassignedRequests final : RequestCouplingConstraint
{
    explicit UnassignedRequests(double weight
                                = static_cast<double>(kMediumUnit))
        : RequestCouplingConstraint("unassignedRequests",
                                    weight,
                                    /*hard=*/false)
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

#endif  // PDP_CONSTRAINTS_UNASSIGNED_REQUESTS_HPP

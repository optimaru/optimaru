#ifndef PDP_CONSTRAINTS_VEHICLE_CAPACITY_HPP
#define PDP_CONSTRAINTS_VEHICLE_CAPACITY_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard (default, weight 1): vehicleCapacity — the running load along a route
// exceeding what the vehicle can carry. Load rises at a pickup and falls at
// the matching dropoff, so what has to fit is the PEAK in between, not the
// route total — and the overshoot is charged at every node it persists
// through, exactly like the full recompute.
struct VehicleCapacity final : PdpUnitConstraint
{
    explicit VehicleCapacity(double weight = 1.0)
        : PdpUnitConstraint("vehicleCapacity", weight, /*hard=*/true)
    {
    }
    double cost(const PdpRoutePlan &plan,
                int unit,
                const std::vector<int> &members) const override
    {
        if (detail::isPool(plan, unit))
            return 0.0;  // nothing is carried in the pool
        const std::int64_t capacity = plan.vehicles[unit].capacity;
        std::int64_t load = 0;
        std::int64_t excess = 0;
        for (int nodeIndex : members)
        {
            const Node &node = plan.nodes[static_cast<std::size_t>(nodeIndex)];
            const Delivery &delivery
                = plan.deliveries[static_cast<std::size_t>(node.deliveryIndex)];
            load += node.pickup ? delivery.demand : -delivery.demand;
            if (load > capacity)
                excess += load - capacity;
        }
        return static_cast<double>(excess);
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_VEHICLE_CAPACITY_HPP

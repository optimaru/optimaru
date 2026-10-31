#ifndef PDP_CONSTRAINTS_VEHICLE_CAPACITY_COST_HPP
#define PDP_CONSTRAINTS_VEHICLE_CAPACITY_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard, weight 1: vehicleCapacityCost — the running load along a route
// exceeding what the vehicle can carry. Load rises at a pickup and falls at
// the matching dropoff, so what has to fit is the PEAK in between, not the
// route total — and the overshoot is charged at every node it persists
// through, exactly like the full recompute. Raw cost per vehicle = the sum
// over its nodes of max(0, load after the node - capacity), in demand units;
// 0 = the load never exceeds the capacity (the pool list costs 0).
struct VehicleCapacityCost final : PdpUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit VehicleCapacityCost(double weight = kDefaultWeight)
        : PdpUnitConstraint("vehicleCapacityCost", weight, kDefaultLevel)
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

#endif  // PDP_CONSTRAINTS_VEHICLE_CAPACITY_COST_HPP

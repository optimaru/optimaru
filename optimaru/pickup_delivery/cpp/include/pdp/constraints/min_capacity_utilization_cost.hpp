#ifndef PDP_CONSTRAINTS_MIN_CAPACITY_UTILIZATION_COST_HPP
#define PDP_CONSTRAINTS_MIN_CAPACITY_UTILIZATION_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Soft, weight 1: minCapacityUtilizationCost — a used vehicle carrying less
// than its Vehicle::minLoadRatio share of its capacity. Raw cost per vehicle
// = max(0, capacity * minLoadRatio - delivered demand), where delivered demand
// is the sum of its pickups' demand, while the vehicle is used and the ratio
// > 0; 0 = on target, unused, or no target set (the pool list costs 0).
struct MinCapacityUtilizationCost final : detail::PerVehicleConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit MinCapacityUtilizationCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("minCapacityUtilizationCost", weight, kDefaultLevel)
    {
    }
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        if (detail::isPool(p, u) || m.empty()
            || p.vehicles[u].minLoadRatio <= 0)
            return 0;
        std::int64_t delivered = 0;
        for (int i : m)
        {
            const Node &n = p.nodes[i];
            if (n.pickup)
                delivered += p.deliveries[n.deliveryIndex].demand;
        }
        return std::max(0.0,
                        p.vehicles[u].capacity * p.vehicles[u].minLoadRatio
                            - delivered);
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_MIN_CAPACITY_UTILIZATION_COST_HPP

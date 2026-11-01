#ifndef PDP_CONSTRAINTS_REQUIRED_VEHICLE_COST_HPP
#define PDP_CONSTRAINTS_REQUIRED_VEHICLE_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard, weight 1000 (ScoreCalculator::kCouplingPenalty): requiredVehicleCost —
// a request promised to a vehicle (Delivery::requiredVehicleIndex) stays on
// it. Unassigned counts once and another vehicle twice, so a request with no
// place on its vehicle waits rather than rides the wrong one. Where on the
// route it goes is still free, which is what pinning its pickup would take
// away.
//
// Raw cost per list = the sum over the bound pickups it holds that are not on
// their vehicle: 1 in the pool, 2 on another vehicle; 0 = every bound request
// rides its vehicle (or nothing is bound).
struct RequiredVehicleCost final : PdpUnitConstraint
{
    static constexpr double kDefaultWeight
        = static_cast<double>(ScoreCalculator::kCouplingPenalty);
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RequiredVehicleCost(double weight = kDefaultWeight)
        : PdpUnitConstraint("requiredVehicleCost", weight, kDefaultLevel)
    {
    }

    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        double misplaced = 0;
        for (int nodeIndex : m)
        {
            const Node &node = p.nodes[static_cast<std::size_t>(nodeIndex)];
            const int required
                = p.deliveries[static_cast<std::size_t>(node.deliveryIndex)]
                      .requiredVehicleIndex;
            if (node.pickup && required >= 0 && required != u)
                misplaced += detail::isPool(p, u) ? 1 : 2;
        }
        return misplaced;
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_REQUIRED_VEHICLE_COST_HPP

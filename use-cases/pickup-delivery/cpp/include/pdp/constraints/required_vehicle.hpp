#ifndef PDP_CONSTRAINTS_REQUIRED_VEHICLE_HPP
#define PDP_CONSTRAINTS_REQUIRED_VEHICLE_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{
// Hard (default, weight kCouplingPenalty): requiredVehicle — a request promised
// to a vehicle stays on it, unassigned included. Where on the route it goes is
// still free, which is what pinning its pickup would take away.
struct RequiredVehicle final : PdpUnitConstraint
{
    RequiredVehicle()
        : PdpUnitConstraint("requiredVehicle",
                            static_cast<double>(ScoreCalculator::kCouplingPenalty),
                            /*hard=*/true)
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
                misplaced += 1;
        }
        return misplaced;
    }
};
}  // namespace pdp
#endif

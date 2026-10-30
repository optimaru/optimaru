#ifndef PDP_CONSTRAINTS_MINIMIZE_LATENESS_HPP
#define PDP_CONSTRAINTS_MINIMIZE_LATENESS_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{
// Sum over requests of the squared seconds from the order to the end of its
// dropoff. Pickups do not count. Squaring per request is the point: one very
// late order costs more than several slightly late ones, so the search spreads
// waiting instead of letting the oldest order pay for everyone else's.
//
// An unassigned request has no dropoff to measure, and charging it nothing would
// make leaving out the oldest orders the cheapest move there is. So it is charged
// as if it finished kUnassignedFinish into the plan.
struct MinimizeLateness final : PdpUnitConstraint
{
    static constexpr Seconds kUnassignedFinish = 3600;

    MinimizeLateness() : PdpUnitConstraint("minimizeLateness", 0.0, false) {}

    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        double total = 0;
        const auto charge = [&](const Node &node, Seconds finish)
        {
            const Delivery &delivery
                = p.deliveries[static_cast<std::size_t>(node.deliveryIndex)];
            const auto age = static_cast<double>(finish - delivery.orderTime);
            total += age * age;
        };
        if (detail::isPool(p, u))
        {
            for (int nodeIndex : m)
            {
                const Node &node = p.nodes[static_cast<std::size_t>(nodeIndex)];
                if (!node.pickup)
                    charge(node, kUnassignedFinish);
            }
            return total;
        }
        detail::walkRoute(
            p,
            u,
            m,
            [&](int nodeIndex, Seconds arrival)
            {
                const Node &node = p.nodes[static_cast<std::size_t>(nodeIndex)];
                if (!node.pickup)
                    charge(node,
                           std::max(arrival, node.minStartTime)
                               + node.serviceDuration);
            });
        return total;
    }
};
}  // namespace pdp
#endif

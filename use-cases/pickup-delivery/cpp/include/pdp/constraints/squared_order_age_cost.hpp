#ifndef PDP_CONSTRAINTS_SQUARED_ORDER_AGE_COST_HPP
#define PDP_CONSTRAINTS_SQUARED_ORDER_AGE_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Soft, weight 0 (opt-in, default_enabled = false): squaredOrderAgeCost — the
// sum over requests of the squared seconds from the order to the end of its
// dropoff. Pickups do not count. Squaring per request is the point: one very
// late order costs more than several slightly late ones, so the search spreads
// waiting instead of letting the oldest order pay for everyone else's.
//
// Raw cost per vehicle = the sum over its dropoffs of (finish -
// Delivery::orderTime)^2 with finish = max(arrival, minStartTime) + service,
// in seconds squared. It is an order AGE, not lateness past a due time, so it
// is 0 only for a dropoff that finishes at its order time. The square is part
// of the raw cost, so an exponent compounds it (exponent 2 = 4th power).
//
// An unassigned request has no dropoff to measure, and charging it nothing would
// make leaving out the oldest orders the cheapest move there is. So the pool
// list charges each of its dropoffs as if it finished kUnassignedFinish into
// the plan.
struct SquaredOrderAgeCost final : PdpUnitConstraint
{
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;
    static constexpr Seconds kUnassignedFinish = 3600;

    explicit SquaredOrderAgeCost(double weight = kDefaultWeight)
        : PdpUnitConstraint("squaredOrderAgeCost", weight, kDefaultLevel)
    {
    }

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

#endif  // PDP_CONSTRAINTS_SQUARED_ORDER_AGE_COST_HPP

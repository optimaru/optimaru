#ifndef PDP_CONSTRAINTS_MAX_END_TIME_COST_HPP
#define PDP_CONSTRAINTS_MAX_END_TIME_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard, weight 1: maxEndTimeCost — the minutes a node's service runs past the
// end of its time window (Node::maxEndTime), rounded up. Raw cost per vehicle
// = the sum over its nodes of ceilToMinutes(arrival + serviceDuration -
// maxEndTime); 0 = every stop finishes inside its window (the pool list
// costs 0). The vehicle's own end-of-shift bound is vehicleMaxEndTimeCost.
struct MaxEndTimeCost final : detail::PerVehicleConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxEndTimeCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("maxEndTimeCost", weight, kDefaultLevel)
    {
    }
    double cost(const PdpRoutePlan &plan,
                int unit,
                const std::vector<int> &members) const override
    {
        if (detail::isPool(plan, unit))
            return 0.0;  // no arrival times there
        std::int64_t late = 0;
        detail::walkRoute(
            plan,
            unit,
            members,
            [&](int nodeIndex, Seconds arrival)
            {
                const Node &node
                    = plan.nodes[static_cast<std::size_t>(nodeIndex)];
                const Seconds finished = arrival + node.serviceDuration;
                if (finished > node.maxEndTime)
                    late += ScoreCalculator::ceilToMinutes(finished
                                                           - node.maxEndTime);
            });
        return static_cast<double>(late);
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_MAX_END_TIME_COST_HPP

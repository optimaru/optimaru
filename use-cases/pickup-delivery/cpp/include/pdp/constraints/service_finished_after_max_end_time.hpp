#ifndef PDP_CONSTRAINTS_SERVICE_FINISHED_AFTER_MAX_END_TIME_HPP
#define PDP_CONSTRAINTS_SERVICE_FINISHED_AFTER_MAX_END_TIME_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard (default, weight 1): serviceFinishedAfterMaxEndTime — the minutes a
// node's service runs past the end of its time window, rounded up.
struct ServiceFinishedAfterMaxEndTime final : PdpUnitConstraint
{
    explicit ServiceFinishedAfterMaxEndTime(double weight = 1.0)
        : PdpUnitConstraint("serviceFinishedAfterMaxEndTime",
                            weight,
                            /*hard=*/true)
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

#endif  // PDP_CONSTRAINTS_SERVICE_FINISHED_AFTER_MAX_END_TIME_HPP

#ifndef PDP_CONSTRAINTS_MINIMIZE_TRAVEL_TIME_HPP
#define PDP_CONSTRAINTS_MINIMIZE_TRAVEL_TIME_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Soft (default, weight 1): minimizeTravelTime — each vehicle's driving
// seconds for the whole route, home -> nodes..., plus the leg back home when
// the vehicle returns to its depot. A vehicle that stays parked drives
// nothing.
struct MinimizeTravelTime final : PdpUnitConstraint
{
    explicit MinimizeTravelTime(double weight = 1.0)
        : PdpUnitConstraint("minimizeTravelTime", weight, /*hard=*/false)
    {
    }
    double cost(const PdpRoutePlan &plan,
                int unit,
                const std::vector<int> &members) const override
    {
        if (detail::isPool(plan, unit))
            return 0.0;
        return static_cast<double>(
            detail::walkRoute(plan, unit, members, [](int, Seconds) {}));
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_MINIMIZE_TRAVEL_TIME_HPP

#ifndef PDP_CONSTRAINTS_ROUTE_DURATION_COST_HPP
#define PDP_CONSTRAINTS_ROUTE_DURATION_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Soft, weight 0 (opt-in, default_enabled = false): routeDurationCost — the
// working time of each used vehicle. Raw cost per vehicle = endTime -
// departureTime in seconds (waiting and service included; the leg home only
// with returnToDepot); 0 for an unused vehicle (and for the pool list).
struct RouteDurationCost final : PdpUnitConstraint
{
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit RouteDurationCost(double weight = kDefaultWeight)
        : PdpUnitConstraint("routeDurationCost", weight, kDefaultLevel)
    {
    }
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        if (detail::isPool(p, u) || m.empty())
            return 0;
        return static_cast<double>(detail::routeMetrics(p, u, m).endTime
                                   - p.vehicles[u].departureTime);
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_ROUTE_DURATION_COST_HPP

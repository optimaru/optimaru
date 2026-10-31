#ifndef PDP_CONSTRAINTS_MAX_ROUTE_RADIUS_COST_HPP
#define PDP_CONSTRAINTS_MAX_ROUTE_RADIUS_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Hard, weight 1: maxRouteRadiusCost — the farthest stop's straight-line
// (haversine) distance from the vehicle's home beyond
// Vehicle::maxRouteRadiusMeters. Raw cost per vehicle = max(0, farthest
// distance - limit) meters while the limit > 0; 0 = every stop within the
// radius, or no limit set (the pool list costs 0).
struct MaxRouteRadiusCost final : PdpUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxRouteRadiusCost(double weight = kDefaultWeight)
        : PdpUnitConstraint("maxRouteRadiusCost", weight, kDefaultLevel)
    {
    }
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        if (detail::isPool(p, u) || p.vehicles[u].maxRouteRadiusMeters <= 0)
            return 0;
        const Location &home = p.locations[p.vehicles[u].homeLocationIndex];
        double furthest = 0;
        for (int i : m)
            furthest
                = std::max(furthest,
                           detail::haversineMeters(
                               home, p.locations[p.nodes[i].locationIndex]));
        return std::max(0.0, furthest - p.vehicles[u].maxRouteRadiusMeters);
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_MAX_ROUTE_RADIUS_COST_HPP

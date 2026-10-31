#ifndef VRP_CONSTRAINTS_MAX_ROUTE_RADIUS_COST_HPP
#define VRP_CONSTRAINTS_MAX_ROUTE_RADIUS_COST_HPP

#include "vrp/constraints/common.hpp"
#include "vrp/constraints/route_geometry.hpp"

namespace vrp {

// Hard, weight 1: maxRouteRadiusCost -- how far a route reaches beyond
// Vehicle::maxRouteRadiusMeters from the vehicle's home. Raw cost per vehicle
// = max(0, haversine meters from home to its farthest visit -
// maxRouteRadiusMeters) while maxRouteRadiusMeters > 0; 0 = every visit
// within the radius, or no radius set (the pool list costs 0).
struct MaxRouteRadiusCost final : VrpUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxRouteRadiusCost(double weight = kDefaultWeight)
        : VrpUnitConstraint("maxRouteRadiusCost", weight, kDefaultLevel) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u) || p.vehicles[u].maxRouteRadiusMeters <= 0) return 0;
        const Location& home = p.locations[p.vehicles[u].homeLocationIndex];
        double furthest = 0;
        for (int i : m) furthest = std::max(furthest,
            detail::haversineMeters(home, p.locations[p.visits[i].locationIndex]));
        return std::max(0.0, furthest - p.vehicles[u].maxRouteRadiusMeters);
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_MAX_ROUTE_RADIUS_COST_HPP

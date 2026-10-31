#ifndef VRP_CONSTRAINTS_ENCLOSING_CIRCLE_RADIUS_COST_HPP
#define VRP_CONSTRAINTS_ENCLOSING_CIRCLE_RADIUS_COST_HPP

#include "vrp/constraints/common.hpp"
#include "vrp/constraints/route_geometry.hpp"

namespace vrp
{

// Soft, weight 0 (opt-in, default_enabled = false): enclosingCircleRadiusCost
// -- how spread out a route is. Raw cost per vehicle = the radius in meters of
// the smallest circle enclosing its visits (the depot is deliberately not a
// point, unlike maxRouteRadiusCost which measures from the depot); 0 for a
// route with fewer than two visits (and for the pool list). Opt-in so adding
// it does not change the default quickstart: give it a weight to prefer
// compact routes.
struct EnclosingCircleRadiusCost final : VrpUnitConstraint
{
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit EnclosingCircleRadiusCost(double weight = kDefaultWeight)
        : VrpUnitConstraint("enclosingCircleRadiusCost", weight, kDefaultLevel)
    {
    }
    double cost(const VehicleRoutePlan &p,
                int u,
                const std::vector<int> &members) const override
    {
        if (detail::isPool(p, u) || members.size() < 2)
            return 0.0;
        std::vector<Location> locations;
        locations.reserve(members.size());
        for (int i : members)
            locations.push_back(p.locations[p.visits[i].locationIndex]);
        return detail::smallestEnclosingRadius(locations);
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_ENCLOSING_CIRCLE_RADIUS_COST_HPP

#ifndef PDP_CONSTRAINTS_ENCLOSING_CIRCLE_RADIUS_COST_HPP
#define PDP_CONSTRAINTS_ENCLOSING_CIRCLE_RADIUS_COST_HPP

#include "pdp/constraints/common.hpp"
#include "pdp/constraints/route_geometry.hpp"

namespace pdp {

// Soft, weight 0 (opt-in, default_enabled = false): enclosingCircleRadiusCost
// — how spread out a route is. Raw cost per vehicle = the radius in meters of
// the smallest circle enclosing its stop points (home excluded); 0 for a
// route with fewer than two stops (and for the pool list). Give it a weight
// to prefer compact routes.
struct EnclosingCircleRadiusCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit EnclosingCircleRadiusCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("enclosingCircleRadiusCost", weight, kDefaultLevel) {}
    double cost(const PdpRoutePlan& p, int u, const std::vector<int>& members) const override {
        if (detail::isPool(p, u) || members.size() < 2) return 0.0;
        std::vector<Location> locations;
        locations.reserve(members.size());
        for (int i : members) locations.push_back(p.locations[p.nodes[i].locationIndex]);
        return detail::smallestEnclosingRadius(locations);
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_ENCLOSING_CIRCLE_RADIUS_COST_HPP

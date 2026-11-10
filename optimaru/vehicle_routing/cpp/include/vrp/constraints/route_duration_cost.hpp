#ifndef VRP_CONSTRAINTS_ROUTE_DURATION_COST_HPP
#define VRP_CONSTRAINTS_ROUTE_DURATION_COST_HPP

#include "vrp/constraints/common.hpp"

namespace vrp {

// Soft, weight 0 (opt-in, default_enabled = false): routeDurationCost -- how
// long each used route takes. Raw cost per vehicle = route end time (the last
// visit's departure, or back home with returnToDepot) - departure time, in
// seconds; 0 = an unused vehicle (and the pool list). Give it a weight to
// prefer short working days.
struct RouteDurationCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit RouteDurationCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("routeDurationCost", weight, kDefaultLevel) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u) || m.empty()) return 0;
        return static_cast<double>(detail::routeMetrics(p, u, m).endTime -
                                   p.vehicles[u].departureTime);
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_ROUTE_DURATION_COST_HPP

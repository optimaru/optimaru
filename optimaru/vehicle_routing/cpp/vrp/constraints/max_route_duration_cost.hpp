#ifndef VRP_CONSTRAINTS_MAX_ROUTE_DURATION_COST_HPP
#define VRP_CONSTRAINTS_MAX_ROUTE_DURATION_COST_HPP

#include "vrp/constraints/common.hpp"

namespace vrp {

// Hard, weight 1: maxRouteDurationCost -- route duration (departure to the
// route's end: the last visit's departure, or back home with returnToDepot)
// above Vehicle::maxRouteDuration. Raw cost per vehicle = max(0, end time -
// departure time - maxRouteDuration) seconds while maxRouteDuration > 0;
// 0 = within the limit, or no limit set (the pool list costs 0).
struct MaxRouteDurationCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxRouteDurationCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("maxRouteDurationCost", weight, kDefaultLevel) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u) || p.vehicles[u].maxRouteDuration <= 0) return 0;
        const auto x = detail::routeMetrics(p, u, m);
        return static_cast<double>(std::max<Seconds>(
                0, x.endTime - p.vehicles[u].departureTime - p.vehicles[u].maxRouteDuration));
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_MAX_ROUTE_DURATION_COST_HPP

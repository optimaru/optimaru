#ifndef PDP_CONSTRAINTS_MAX_ROUTE_DURATION_COST_HPP
#define PDP_CONSTRAINTS_MAX_ROUTE_DURATION_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp {

// Hard, weight 1: maxRouteDurationCost — route duration (departure to route
// end, waiting and service included; the leg home only with returnToDepot)
// above Vehicle::maxRouteDuration. Raw cost per vehicle = max(0, endTime -
// departureTime - maxRouteDuration) seconds while maxRouteDuration > 0;
// 0 = within the limit, or no limit set (the pool list costs 0).
struct MaxRouteDurationCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxRouteDurationCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("maxRouteDurationCost", weight, kDefaultLevel) {}
    double cost(const PdpRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u) || p.vehicles[u].maxRouteDuration <= 0) return 0;
        const auto x = detail::routeMetrics(p, u, m);
        return static_cast<double>(std::max<Seconds>(
                0, x.endTime - p.vehicles[u].departureTime - p.vehicles[u].maxRouteDuration));
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_MAX_ROUTE_DURATION_COST_HPP

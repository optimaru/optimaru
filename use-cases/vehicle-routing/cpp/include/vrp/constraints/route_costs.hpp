#ifndef VRP_CONSTRAINTS_ROUTE_COSTS_HPP
#define VRP_CONSTRAINTS_ROUTE_COSTS_HPP

#include "vrp/constraints/common.hpp"

namespace vrp {
// Disabled by weight 0 in the default set: opt in with weight=1 (or another
// business weight), for duration and latency constraints.
struct MinimizeRouteDuration final : VrpUnitConstraint {
    MinimizeRouteDuration() : VrpUnitConstraint("minimizeRouteDuration", 0.0, false) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u) || m.empty()) return 0;
        return static_cast<double>(detail::routeMetrics(p, u, m).endTime
                                   - p.vehicles[u].departureTime);
    }
};
struct MinimizeLatency final : VrpUnitConstraint {
    MinimizeLatency() : VrpUnitConstraint("minimizeLatency", 0.0, false) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u)) return 0;
        return static_cast<double>(detail::routeMetrics(p, u, m).latency);
    }
};
struct VehicleFixedCost final : VrpUnitConstraint {
    VehicleFixedCost() : VrpUnitConstraint("vehicleFixedCost", 1.0, false) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        return detail::isPool(p, u) || m.empty() ? 0.0 : p.vehicles[u].fixedCost;
    }
};
}  // namespace vrp
#endif

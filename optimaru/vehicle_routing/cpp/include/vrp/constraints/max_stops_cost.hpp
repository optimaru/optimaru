#ifndef VRP_CONSTRAINTS_MAX_STOPS_COST_HPP
#define VRP_CONSTRAINTS_MAX_STOPS_COST_HPP

#include "vrp/constraints/common.hpp"

namespace vrp {

// Hard, weight 1: maxStopsCost -- visits on a route beyond Vehicle::maxStops.
// Raw cost per vehicle = max(0, visits on the route - maxStops) while
// maxStops > 0; 0 = within the limit, or no limit set (the pool list costs
// 0).
struct MaxStopsCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxStopsCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("maxStopsCost", weight, kDefaultLevel) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u) || p.vehicles[u].maxStops <= 0) return 0;
        return static_cast<double>(
                std::max(0, static_cast<int>(m.size()) - p.vehicles[u].maxStops));
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_MAX_STOPS_COST_HPP

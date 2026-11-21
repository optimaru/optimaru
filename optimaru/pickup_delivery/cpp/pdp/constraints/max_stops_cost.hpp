#ifndef PDP_CONSTRAINTS_MAX_STOPS_COST_HPP
#define PDP_CONSTRAINTS_MAX_STOPS_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp {

// Hard, weight 1: maxStopsCost — pickup and dropoff stops above
// Vehicle::maxStops (a request counts as two). Raw cost per vehicle =
// max(0, stop count - maxStops) while maxStops > 0; 0 = within the cap, or no
// cap set (the pool list costs 0).
struct MaxStopsCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxStopsCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("maxStopsCost", weight, kDefaultLevel) {}
    double cost(const PdpRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u) || p.vehicles[u].maxStops <= 0) return 0;
        return static_cast<double>(
                std::max(0, static_cast<int>(m.size()) - p.vehicles[u].maxStops));
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_MAX_STOPS_COST_HPP

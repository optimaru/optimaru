#ifndef VRP_CONSTRAINTS_LATENCY_COST_HPP
#define VRP_CONSTRAINTS_LATENCY_COST_HPP

#include "vrp/constraints/common.hpp"

namespace vrp {

// Soft, weight 0 (opt-in, default_enabled = false): latencyCost -- how long
// customers wait for their vehicle. Raw cost per vehicle = the sum over its
// visits of max(0, arrival time - vehicle departure time), in seconds; 0 = an
// unused vehicle (and the pool list). Give it a weight to serve visits early
// in the route.
struct LatencyCost final : VrpUnitConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit LatencyCost(double weight = kDefaultWeight)
        : VrpUnitConstraint("latencyCost", weight, kDefaultLevel) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u)) return 0;
        return static_cast<double>(detail::routeMetrics(p, u, m).latency);
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_LATENCY_COST_HPP

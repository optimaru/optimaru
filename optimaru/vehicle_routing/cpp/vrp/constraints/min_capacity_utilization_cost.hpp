#ifndef VRP_CONSTRAINTS_MIN_CAPACITY_UTILIZATION_COST_HPP
#define VRP_CONSTRAINTS_MIN_CAPACITY_UTILIZATION_COST_HPP

#include "vrp/constraints/common.hpp"

namespace vrp {

// Soft, weight 1: minCapacityUtilizationCost -- how far a used vehicle's load
// falls short of Vehicle::minLoadRatio of its capacity. Raw cost per vehicle =
// max(0, capacity * minLoadRatio - summed demand) in demand units, for a
// vehicle with at least one visit and minLoadRatio > 0; 0 = loaded enough,
// unused, or no ratio set (the pool list costs 0).
struct MinCapacityUtilizationCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit MinCapacityUtilizationCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("minCapacityUtilizationCost", weight, kDefaultLevel) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u) || m.empty() || p.vehicles[u].minLoadRatio <= 0) return 0;
        std::int64_t load = 0;
        for (int i : m) load += p.visits[i].demand;
        return std::max(0.0, p.vehicles[u].capacity * p.vehicles[u].minLoadRatio - load);
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_MIN_CAPACITY_UTILIZATION_COST_HPP

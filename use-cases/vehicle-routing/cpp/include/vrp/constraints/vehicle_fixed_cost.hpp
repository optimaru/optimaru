#ifndef VRP_CONSTRAINTS_VEHICLE_FIXED_COST_HPP
#define VRP_CONSTRAINTS_VEHICLE_FIXED_COST_HPP

#include "vrp/constraints/common.hpp"

namespace vrp {

// Soft, weight 1: vehicleFixedCost -- the price of putting a vehicle on the
// road. Raw cost per vehicle = Vehicle::fixedCost when it serves at least one
// visit; 0 = an unused vehicle (and the pool list).
struct VehicleFixedCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit VehicleFixedCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("vehicleFixedCost", weight, kDefaultLevel) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        return detail::isPool(p, u) || m.empty() ? 0.0 : p.vehicles[u].fixedCost;
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_VEHICLE_FIXED_COST_HPP

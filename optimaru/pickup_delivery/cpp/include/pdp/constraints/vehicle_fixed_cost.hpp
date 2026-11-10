#ifndef PDP_CONSTRAINTS_VEHICLE_FIXED_COST_HPP
#define PDP_CONSTRAINTS_VEHICLE_FIXED_COST_HPP

#include "pdp/constraints/common.hpp"

namespace pdp
{

// Soft, weight 1: vehicleFixedCost — the price of putting a vehicle on the
// road at all. Raw cost per vehicle = Vehicle::fixedCost while it serves at
// least one stop; 0 for an unused vehicle (and for the pool list).
struct VehicleFixedCost final : detail::PerVehicleConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit VehicleFixedCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("vehicleFixedCost", weight, kDefaultLevel)
    {
    }
    double
    cost(const PdpRoutePlan &p, int u, const std::vector<int> &m) const override
    {
        return detail::isPool(p, u) || m.empty() ? 0.0
                                                 : p.vehicles[u].fixedCost;
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_VEHICLE_FIXED_COST_HPP

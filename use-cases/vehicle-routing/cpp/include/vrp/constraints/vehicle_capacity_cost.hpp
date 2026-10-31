#ifndef VRP_CONSTRAINTS_VEHICLE_CAPACITY_COST_HPP
#define VRP_CONSTRAINTS_VEHICLE_CAPACITY_COST_HPP

#include "vrp/constraints/route_sums.hpp"

namespace vrp {

// Hard, weight 1: vehicleCapacityCost -- the demand a vehicle carries beyond
// what it can hold. Raw cost per vehicle = max(0, summed demand of its visits
// - capacity); 0 = within capacity (the pool list costs 0). Previewed in
// O(pieces) from the shared load sums.
struct VehicleCapacityCost final : detail::SummedUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit VehicleCapacityCost(double weight = kDefaultWeight)
        : SummedUnitConstraint("vehicleCapacityCost", weight, kDefaultLevel) {}

    bool piecewiseCost(const VehicleRoutePlan& plan, int unit, const optimaru::ListPiece* pieces,
                       int count, double& out) const override {
        if (detail::isPool(plan, unit)) {
            out = 0.0;
            return true;
        }
        std::int64_t demand = 0;
        for (int i = 0; i < count; ++i) demand += sums_->loadOf(pieces[i]);
        const std::int64_t over = demand - plan.vehicles[unit].capacity;
        out = over > 0 ? static_cast<double>(over) : 0.0;
        return true;
    }

    double cost(const VehicleRoutePlan& plan, int unit,
                const std::vector<int>& members) const override {
        if (detail::isPool(plan, unit)) return 0.0;  // the unassigned pool
        std::int64_t demand = 0;
        for (int visitIndex : members) demand += plan.visits[visitIndex].demand;
        const std::int64_t over = demand - plan.vehicles[unit].capacity;
        return over > 0 ? static_cast<double>(over) : 0.0;
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_VEHICLE_CAPACITY_COST_HPP

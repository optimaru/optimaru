#ifndef VRP_CONSTRAINTS_VEHICLE_CAPACITY_HPP
#define VRP_CONSTRAINTS_VEHICLE_CAPACITY_HPP

#include "vrp/constraints/route_sums.hpp"

namespace vrp {

// Hard (default, weight 1): vehicleCapacity -- the demand a vehicle carries
// beyond what it can hold. Previewed in O(pieces) from the shared load sums.
struct VehicleCapacity final : detail::SummedUnitConstraint {
    explicit VehicleCapacity(double weight = 1.0)
        : SummedUnitConstraint("vehicleCapacity", weight, /*hard=*/true) {}

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

#endif  // VRP_CONSTRAINTS_VEHICLE_CAPACITY_HPP

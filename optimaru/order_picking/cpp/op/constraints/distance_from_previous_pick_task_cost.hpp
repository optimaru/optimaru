#ifndef OP_CONSTRAINTS_DISTANCE_FROM_PREVIOUS_PICK_TASK_COST_HPP
#define OP_CONSTRAINTS_DISTANCE_FROM_PREVIOUS_PICK_TASK_COST_HPP

#include "op/constraints/common.hpp"

namespace op {

// Soft, weight 1: distanceFromPreviousPickTaskCost — the walk from each pick
// to the next one, with the trolley's own parking spot standing in as the
// "previous" location of the first pick. Raw cost per trolley = the summed
// warehouse distance (m) start -> first pick -> ... -> last pick, without the
// return leg; 0 for an empty trolley (or picks all on its parking spot).
struct DistanceFromPreviousPickTaskCost final : OpUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit DistanceFromPreviousPickTaskCost(double weight = kDefaultWeight)
        : OpUnitConstraint("distanceFromPreviousPickTaskCost", weight, kDefaultLevel) {}
    double cost(const OrderPickingSolution& solution, int unit,
                const std::vector<int>& members) const override {
        const WarehouseLocation* previous = &solution.trolleys[unit].location;
        std::int64_t distance = 0;
        for (int pickIndex : members) {
            const WarehouseLocation& location = solution.pickTasks[pickIndex].location;
            distance += calculateDistance(*previous, location);
            previous = &location;
        }
        return static_cast<double>(distance);
    }
};

}  // namespace op

#endif  // OP_CONSTRAINTS_DISTANCE_FROM_PREVIOUS_PICK_TASK_COST_HPP

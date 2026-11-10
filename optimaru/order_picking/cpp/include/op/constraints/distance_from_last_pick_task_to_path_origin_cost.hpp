#ifndef OP_CONSTRAINTS_DISTANCE_FROM_LAST_PICK_TASK_TO_PATH_ORIGIN_COST_HPP
#define OP_CONSTRAINTS_DISTANCE_FROM_LAST_PICK_TASK_TO_PATH_ORIGIN_COST_HPP

#include "op/constraints/common.hpp"

namespace op {

// Soft, weight 1: distanceFromLastPickTaskToPathOriginCost — the walk back
// from the trolley's final pick to where it started. Raw cost per trolley =
// the warehouse distance (m) from its last pick to its start location; an
// empty trolley never leaves, so it costs 0.
struct DistanceFromLastPickTaskToPathOriginCost final : OpUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit DistanceFromLastPickTaskToPathOriginCost(double weight = kDefaultWeight)
        : OpUnitConstraint("distanceFromLastPickTaskToPathOriginCost", weight, kDefaultLevel) {}
    double cost(const OrderPickingSolution& solution, int unit,
                const std::vector<int>& members) const override {
        if (members.empty()) return 0.0;
        return static_cast<double>(calculateDistance(solution.pickTasks[members.back()].location,
                                                     solution.trolleys[unit].location));
    }
};

}  // namespace op

#endif  // OP_CONSTRAINTS_DISTANCE_FROM_LAST_PICK_TASK_TO_PATH_ORIGIN_COST_HPP

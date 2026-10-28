#ifndef OP_CONSTRAINTS_MINIMIZE_DISTANCE_FROM_LAST_PICK_TASK_HPP
#define OP_CONSTRAINTS_MINIMIZE_DISTANCE_FROM_LAST_PICK_TASK_HPP

#include "op/constraints/common.hpp"

namespace op
{

// Soft (default, weight 1): minimizeDistanceFromLastPickTaskToPathOrigin —
// the walk back from the trolley's final pick to where it started. An empty
// trolley never leaves, so it costs nothing.
struct MinimizeDistanceFromLastPickTaskToPathOrigin final : OpUnitConstraint
{
    explicit MinimizeDistanceFromLastPickTaskToPathOrigin(double weight = 1.0)
        : OpUnitConstraint("minimizeDistanceFromLastPickTaskToPathOrigin",
                           weight,
                           /*hard=*/false)
    {
    }
    double cost(const OrderPickingSolution &solution,
                int unit,
                const std::vector<int> &members) const override
    {
        if (members.empty())
            return 0.0;
        return static_cast<double>(
            calculateDistance(solution.pickTasks[members.back()].location,
                              solution.trolleys[unit].location));
    }
};

}  // namespace op

#endif  // OP_CONSTRAINTS_MINIMIZE_DISTANCE_FROM_LAST_PICK_TASK_HPP

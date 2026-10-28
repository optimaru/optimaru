#ifndef OP_CONSTRAINTS_MINIMIZE_DISTANCE_FROM_PREVIOUS_PICK_TASK_HPP
#define OP_CONSTRAINTS_MINIMIZE_DISTANCE_FROM_PREVIOUS_PICK_TASK_HPP

#include "op/constraints/common.hpp"

namespace op
{

// Soft (default, weight 1): minimizeDistanceFromPreviousPickTask — the walk
// from each pick to the next one, with the trolley's own parking spot standing
// in as the "previous" location of the first pick.
struct MinimizeDistanceFromPreviousPickTask final : OpUnitConstraint
{
    explicit MinimizeDistanceFromPreviousPickTask(double weight = 1.0)
        : OpUnitConstraint("minimizeDistanceFromPreviousPickTask",
                           weight,
                           /*hard=*/false)
    {
    }
    double cost(const OrderPickingSolution &solution,
                int unit,
                const std::vector<int> &members) const override
    {
        const WarehouseLocation *previous = &solution.trolleys[unit].location;
        std::int64_t distance = 0;
        for (int pickIndex : members)
        {
            const WarehouseLocation &location
                = solution.pickTasks[pickIndex].location;
            distance += calculateDistance(*previous, location);
            previous = &location;
        }
        return static_cast<double>(distance);
    }
};

}  // namespace op

#endif  // OP_CONSTRAINTS_MINIMIZE_DISTANCE_FROM_PREVIOUS_PICK_TASK_HPP

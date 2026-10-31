#ifndef TA_CONSTRAINTS_MINOR_PRIORITY_TASK_END_TIME_COST_HPP
#define TA_CONSTRAINTS_MINOR_PRIORITY_TASK_END_TIME_COST_HPP

#include "ta/constraints/common.hpp"

namespace ta
{

// Soft, weight 1: minorPriorityTaskEndTimeCost — the end time of every MINOR
// task, the baseline of the three priority rules. Raw cost per employee = the
// summed end times (minutes) of its MINOR tasks, so 0 = the employee does no
// MINOR task. The Bendable soft[2] level.
struct MinorPriorityTaskEndTimeCost final : PriorityTaskEndTime
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit MinorPriorityTaskEndTimeCost(double weight = kDefaultWeight)
        : PriorityTaskEndTime("minorPriorityTaskEndTimeCost",
                              weight,
                              kDefaultLevel,
                              Priority::MINOR)
    {
    }
};

}  // namespace ta

#endif  // TA_CONSTRAINTS_MINOR_PRIORITY_TASK_END_TIME_COST_HPP

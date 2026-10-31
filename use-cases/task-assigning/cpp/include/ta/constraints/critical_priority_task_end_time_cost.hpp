#ifndef TA_CONSTRAINTS_CRITICAL_PRIORITY_TASK_END_TIME_COST_HPP
#define TA_CONSTRAINTS_CRITICAL_PRIORITY_TASK_END_TIME_COST_HPP

#include "ta/constraints/common.hpp"

namespace ta
{

// Soft, weight 4: criticalPriorityTaskEndTimeCost — the end time of every
// CRITICAL task, weighed four times as heavily as a minor one. Raw cost per
// employee = the summed end times (minutes) of its CRITICAL tasks, so 0 = the
// employee does no CRITICAL task. The Bendable soft[2] level.
struct CriticalPriorityTaskEndTimeCost final : PriorityTaskEndTime
{
    static constexpr double kDefaultWeight = 4.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit CriticalPriorityTaskEndTimeCost(double weight = kDefaultWeight)
        : PriorityTaskEndTime("criticalPriorityTaskEndTimeCost",
                              weight,
                              kDefaultLevel,
                              Priority::CRITICAL)
    {
    }
};

}  // namespace ta

#endif  // TA_CONSTRAINTS_CRITICAL_PRIORITY_TASK_END_TIME_COST_HPP

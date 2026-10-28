#ifndef TA_CONSTRAINTS_CRITICAL_PRIORITY_TASK_END_TIME_HPP
#define TA_CONSTRAINTS_CRITICAL_PRIORITY_TASK_END_TIME_HPP

#include "ta/constraints/common.hpp"

namespace ta
{

// Soft (default, weight 4): criticalPriorityTaskEndTime — the end time of
// every CRITICAL task, weighed four times as heavily as a minor one.
struct CriticalPriorityTaskEndTime final : PriorityTaskEndTime
{
    explicit CriticalPriorityTaskEndTime(double weight = 4.0)
        : PriorityTaskEndTime("criticalPriorityTaskEndTime",
                              weight,
                              /*hard=*/false,
                              Priority::CRITICAL)
    {
    }
};

}  // namespace ta

#endif  // TA_CONSTRAINTS_CRITICAL_PRIORITY_TASK_END_TIME_HPP

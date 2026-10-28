#ifndef TA_CONSTRAINTS_MINOR_PRIORITY_TASK_END_TIME_HPP
#define TA_CONSTRAINTS_MINOR_PRIORITY_TASK_END_TIME_HPP

#include "ta/constraints/common.hpp"

namespace ta
{

// Soft (default, weight 1): minorPriorityTaskEndTime — the end time of every
// MINOR task, the baseline of the three priority rules.
struct MinorPriorityTaskEndTime final : PriorityTaskEndTime
{
    explicit MinorPriorityTaskEndTime(double weight = 1.0)
        : PriorityTaskEndTime("minorPriorityTaskEndTime",
                              weight,
                              /*hard=*/false,
                              Priority::MINOR)
    {
    }
};

}  // namespace ta

#endif  // TA_CONSTRAINTS_MINOR_PRIORITY_TASK_END_TIME_HPP

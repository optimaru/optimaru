#ifndef TA_CONSTRAINTS_MAJOR_PRIORITY_TASK_END_TIME_HPP
#define TA_CONSTRAINTS_MAJOR_PRIORITY_TASK_END_TIME_HPP

#include "ta/constraints/common.hpp"

namespace ta
{

// Soft (default, weight 2): majorPriorityTaskEndTime — the end time of every
// MAJOR task, weighed twice as heavily as a minor one.
struct MajorPriorityTaskEndTime final : PriorityTaskEndTime
{
    explicit MajorPriorityTaskEndTime(double weight = 2.0)
        : PriorityTaskEndTime("majorPriorityTaskEndTime",
                              weight,
                              /*hard=*/false,
                              Priority::MAJOR)
    {
    }
};

}  // namespace ta

#endif  // TA_CONSTRAINTS_MAJOR_PRIORITY_TASK_END_TIME_HPP

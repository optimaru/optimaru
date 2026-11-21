#ifndef TA_CONSTRAINTS_MAJOR_PRIORITY_TASK_END_TIME_COST_HPP
#define TA_CONSTRAINTS_MAJOR_PRIORITY_TASK_END_TIME_COST_HPP

#include "ta/constraints/common.hpp"

namespace ta {

// Soft, weight 2: majorPriorityTaskEndTimeCost — the end time of every MAJOR
// task, weighed twice as heavily as a minor one. Raw cost per employee = the
// summed end times (minutes) of its MAJOR tasks, so 0 = the employee does no
// MAJOR task. The soft[2] level.
struct MajorPriorityTaskEndTimeCost final : PriorityTaskEndTimeCostBase {
    static constexpr double kDefaultWeight = 2.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit MajorPriorityTaskEndTimeCost(double weight = kDefaultWeight)
        : PriorityTaskEndTimeCostBase("majorPriorityTaskEndTimeCost", weight, kDefaultLevel,
                                      Priority::MAJOR) {}
};

}  // namespace ta

#endif  // TA_CONSTRAINTS_MAJOR_PRIORITY_TASK_END_TIME_COST_HPP

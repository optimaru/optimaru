#ifndef MS_CONSTRAINTS_AVOID_OVERTIME_HPP
#define MS_CONSTRAINTS_AVOID_OVERTIME_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Hard (default, weight 1): avoidOvertime — a meeting that runs past the end
// of the calendar, i.e. no TimeGrain fact exists at its last grain index. The
// penalty is that last index, so overrunning further costs more.
struct AvoidOvertime final : PerAssignmentConstraint
{
    explicit AvoidOvertime(double weight = 1.0)
        : PerAssignmentConstraint("avoidOvertime", weight, /*hard=*/true)
    {
    }
    double assignmentCost(const MeetingSchedule &schedule,
                          int unit,
                          int assignmentIndex) const override
    {
        int start = 0;
        int end = 0;
        detail::spanOf(schedule, unit, assignmentIndex, start, end);
        const int lastIndex = end - 1;
        if (schedule.grainCountAtOffset(lastIndex - schedule.grainIndexBase)
            != 0)
            return 0.0;
        return static_cast<double>(lastIndex);
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_AVOID_OVERTIME_HPP

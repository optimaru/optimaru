#ifndef MS_CONSTRAINTS_START_AND_END_ON_SAME_DAY_HPP
#define MS_CONSTRAINTS_START_AND_END_ON_SAME_DAY_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Hard (default, weight 1): startAndEndOnSameDay — a meeting whose last grain
// falls on a different day than its first. Counted per TimeGrain fact at the
// last index that sits on another day, which is the Java join's shape (and
// zero when the meeting overruns the calendar entirely — avoidOvertime prices
// that case instead).
struct StartAndEndOnSameDay final : PerAssignmentConstraint
{
    explicit StartAndEndOnSameDay(double weight = 1.0)
        : PerAssignmentConstraint("startAndEndOnSameDay", weight, /*hard=*/true)
    {
    }
    double assignmentCost(const MeetingSchedule &schedule,
                          int unit,
                          int assignmentIndex) const override
    {
        int start = 0;
        int end = 0;
        detail::spanOf(schedule, unit, assignmentIndex, start, end);
        const int lastOffset = end - 1 - schedule.grainIndexBase;
        const int startDay
            = schedule.timeGrains[schedule.timeGrainPosOf(unit)].dayOfYear;
        return static_cast<double>(
            schedule.grainCountAtOffset(lastOffset)
            - schedule.grainCountAtOffsetOnDay(lastOffset, startDay));
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_START_AND_END_ON_SAME_DAY_HPP

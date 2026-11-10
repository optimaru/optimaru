#ifndef MS_CONSTRAINTS_DAY_CROSSING_COST_HPP
#define MS_CONSTRAINTS_DAY_CROSSING_COST_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Hard, weight 1: dayCrossingCost — a meeting whose last grain falls on a
// different day than its first. Raw cost per meeting = the TimeGrains at
// its last grain index that sit on another day than its start (normally 0 or
// 1); 0 = the meeting starts and ends on the
// same day. A meeting that overruns the calendar entirely also costs 0 here —
// overtimeCost prices that case instead.
struct DayCrossingCost final : PerAssignmentConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit DayCrossingCost(double weight = kDefaultWeight)
        : PerAssignmentConstraint("dayCrossingCost", weight, kDefaultLevel)
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

#endif  // MS_CONSTRAINTS_DAY_CROSSING_COST_HPP

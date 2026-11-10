#ifndef MS_CONSTRAINTS_MEETING_END_TIME_COST_HPP
#define MS_CONSTRAINTS_MEETING_END_TIME_COST_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Soft, weight 1: meetingEndTimeCost — how late each meeting ends, which
// pulls the whole schedule toward the front of the calendar. Raw cost per
// meeting = its last grain index; 0 only for a meeting ending on grain 0.
struct MeetingEndTimeCost final : PerAssignmentConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit MeetingEndTimeCost(double weight = kDefaultWeight)
        : PerAssignmentConstraint("meetingEndTimeCost", weight, kDefaultLevel)
    {
    }
    double assignmentCost(const MeetingSchedule &schedule,
                          int unit,
                          int assignmentIndex) const override
    {
        int start = 0;
        int end = 0;
        detail::spanOf(schedule, unit, assignmentIndex, start, end);
        return static_cast<double>(end - 1);
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_MEETING_END_TIME_COST_HPP

#ifndef MS_CONSTRAINTS_DO_MEETINGS_AS_SOON_AS_POSSIBLE_HPP
#define MS_CONSTRAINTS_DO_MEETINGS_AS_SOON_AS_POSSIBLE_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Soft (default, weight 1): doMeetingsAsSoonAsPossible — every meeting
// penalizes its last grain index, which pulls the whole schedule toward the
// front of the calendar.
struct DoMeetingsAsSoonAsPossible final : PerAssignmentConstraint
{
    explicit DoMeetingsAsSoonAsPossible(double weight = 1.0)
        : PerAssignmentConstraint("doMeetingsAsSoonAsPossible",
                                  weight,
                                  /*hard=*/false)
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

#endif  // MS_CONSTRAINTS_DO_MEETINGS_AS_SOON_AS_POSSIBLE_HPP

#ifndef MS_CONSTRAINTS_REQUIRED_AND_PREFERRED_ATTENDANCE_CONFLICT_HPP
#define MS_CONSTRAINTS_REQUIRED_AND_PREFERRED_ATTENDANCE_CONFLICT_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Medium (default, weight kMediumScale): requiredAndPreferredAttendanceConflict
// — one point per overlapping grain per person required at one meeting and
// merely preferred at the other. Both directions count, which is why the arc
// carries a forward and a reverse weight and this component sums them.
struct RequiredAndPreferredAttendanceConflict final : AttendanceArcConstraint
{
    explicit RequiredAndPreferredAttendanceConflict(double weight
                                                    = kMediumScale)
        : AttendanceArcConstraint("requiredAndPreferredAttendanceConflict",
                                  weight,
                                  /*hard=*/false)
    {
    }

protected:
    std::int64_t marginal(const MeetingSchedule &schedule,
                          int assignmentIndex,
                          int unit) const override
    {
        const int meeting
            = schedule.meetingAssignments[assignmentIndex].meeting;
        int start = 0;
        int end = 0;
        detail::spanOf(schedule, unit, assignmentIndex, start, end);
        std::int64_t total
            = schedule.selfRequiredPreferred[meeting] * (end - start);
        for (int arcIndex : arcsOf(meeting))
        {
            const ConflictArc &arc = schedule.conflictArcs[arcIndex];
            const std::int64_t weightBothWays
                = arc.reqPrefForward + arc.reqPrefReverse;
            if (weightBothWays == 0)
                continue;
            int partnerAssignment = -1;
            int partnerUnit = -1;
            if (!partnerOf(
                    schedule, arc, meeting, partnerAssignment, partnerUnit))
                continue;
            int partnerStart = 0;
            int partnerEnd = 0;
            detail::spanOf(schedule,
                           partnerUnit,
                           partnerAssignment,
                           partnerStart,
                           partnerEnd);
            total += weightBothWays
                     * ScoreCalculator::overlapGrains(
                         start, end, partnerStart, partnerEnd);
        }
        return total;
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_REQUIRED_AND_PREFERRED_ATTENDANCE_CONFLICT_HPP

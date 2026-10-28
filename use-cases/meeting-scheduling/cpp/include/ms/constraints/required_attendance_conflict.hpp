#ifndef MS_CONSTRAINTS_REQUIRED_ATTENDANCE_CONFLICT_HPP
#define MS_CONSTRAINTS_REQUIRED_ATTENDANCE_CONFLICT_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Hard (default, weight 1): requiredAttendanceConflict — one point per
// overlapping grain per person required at BOTH meetings. Somebody who must
// attend two meetings at once makes the schedule impossible, hence hard.
//
// The same-meeting term (a meeting listing one person as required twice) is
// folded in here rather than split off, so this component is exactly the
// provider's one rule.
struct RequiredAttendanceConflict final : AttendanceArcConstraint
{
    explicit RequiredAttendanceConflict(double weight = 1.0)
        : AttendanceArcConstraint("requiredAttendanceConflict",
                                  weight,
                                  /*hard=*/true)
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
            = schedule.selfRequiredPairs[meeting] * (end - start);
        for (int arcIndex : arcsOf(meeting))
        {
            const ConflictArc &arc = schedule.conflictArcs[arcIndex];
            if (arc.requiredWeight == 0)
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
            total += arc.requiredWeight
                     * ScoreCalculator::overlapGrains(
                         start, end, partnerStart, partnerEnd);
        }
        return total;
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_REQUIRED_ATTENDANCE_CONFLICT_HPP

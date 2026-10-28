#ifndef MS_CONSTRAINTS_PREFERRED_ATTENDANCE_CONFLICT_HPP
#define MS_CONSTRAINTS_PREFERRED_ATTENDANCE_CONFLICT_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Medium (default, weight kMediumScale): preferredAttendanceConflict — one
// point per overlapping grain per person who would PREFER to attend both
// meetings. Not impossible, just a shame, which is why it sits on the medium
// level below the hard rules and above every soft comfort term.
struct PreferredAttendanceConflict final : AttendanceArcConstraint
{
    explicit PreferredAttendanceConflict(double weight = kMediumScale)
        : AttendanceArcConstraint("preferredAttendanceConflict",
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
            = schedule.selfPreferredPairs[meeting] * (end - start);
        for (int arcIndex : arcsOf(meeting))
        {
            const ConflictArc &arc = schedule.conflictArcs[arcIndex];
            if (arc.preferredWeight == 0)
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
            total += arc.preferredWeight
                     * ScoreCalculator::overlapGrains(
                         start, end, partnerStart, partnerEnd);
        }
        return total;
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_PREFERRED_ATTENDANCE_CONFLICT_HPP

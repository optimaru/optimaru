#ifndef MS_CONSTRAINTS_REQUIRED_ATTENDANCE_CONFLICT_COST_HPP
#define MS_CONSTRAINTS_REQUIRED_ATTENDANCE_CONFLICT_COST_HPP

#include "ms/constraints/common.hpp"

namespace ms {

// Hard, weight 1: requiredAttendanceConflictCost — a person REQUIRED at two
// meetings that overlap. Somebody who must attend two meetings at once makes
// the schedule impossible, hence hard. Raw cost (global) = overlapping grains
// x shared required attendees, summed over meeting pairs (person-grains);
// 0 = nobody required anywhere is double-booked.
//
// The same-meeting term (a meeting listing one person as required twice) is
// folded in here rather than split off, so this component is exactly
// one rule.
struct RequiredAttendanceConflictCost final : AttendanceArcConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RequiredAttendanceConflictCost(double weight = kDefaultWeight)
        : AttendanceArcConstraint("requiredAttendanceConflictCost", weight, kDefaultLevel) {}

protected:
    std::int64_t marginal(const MeetingSchedule& schedule, int assignmentIndex,
                          int unit) const override {
        const int meeting = schedule.meetingAssignments[assignmentIndex].meeting;
        int start = 0;
        int end = 0;
        detail::spanOf(schedule, unit, assignmentIndex, start, end);
        std::int64_t total = schedule.selfRequiredPairs[meeting] * (end - start);
        for (int arcIndex : arcsOf(meeting)) {
            const ConflictArc& arc = schedule.conflictArcs[arcIndex];
            if (arc.requiredWeight == 0) continue;
            int partnerAssignment = -1;
            int partnerUnit = -1;
            if (!partnerOf(schedule, arc, meeting, partnerAssignment, partnerUnit)) continue;
            int partnerStart = 0;
            int partnerEnd = 0;
            detail::spanOf(schedule, partnerUnit, partnerAssignment, partnerStart, partnerEnd);
            total += arc.requiredWeight *
                     ScoreCalculator::overlapGrains(start, end, partnerStart, partnerEnd);
        }
        return total;
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_REQUIRED_ATTENDANCE_CONFLICT_COST_HPP

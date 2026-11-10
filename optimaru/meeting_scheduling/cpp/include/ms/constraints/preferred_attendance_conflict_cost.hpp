#ifndef MS_CONSTRAINTS_PREFERRED_ATTENDANCE_CONFLICT_COST_HPP
#define MS_CONSTRAINTS_PREFERRED_ATTENDANCE_CONFLICT_COST_HPP

#include "ms/constraints/common.hpp"

namespace ms {

// Medium, weight 1 (medium units): preferredAttendanceConflictCost — a
// person who would PREFER to attend two meetings that overlap. Not
// impossible, just a shame, which is why it sits on the medium level below
// the hard rules and above every soft comfort term. Raw cost (global) =
// overlapping grains x shared preferred attendees, summed over meeting pairs
// (person-grains); 0 = no such clash.
//
// Folded into the engine's soft level as `medium * kMediumScale` by the
// calculator (kMediumFold in domain.hpp), not by this weight.
struct PreferredAttendanceConflictCost final : AttendanceArcConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Medium;

    explicit PreferredAttendanceConflictCost(double weight = kDefaultWeight)
        : AttendanceArcConstraint("preferredAttendanceConflictCost", weight, kDefaultLevel) {}

protected:
    std::int64_t marginal(const MeetingSchedule& schedule, int assignmentIndex,
                          int unit) const override {
        const int meeting = schedule.meetingAssignments[assignmentIndex].meeting;
        int start = 0;
        int end = 0;
        detail::spanOf(schedule, unit, assignmentIndex, start, end);
        std::int64_t total = schedule.selfPreferredPairs[meeting] * (end - start);
        for (int arcIndex : arcsOf(meeting)) {
            const ConflictArc& arc = schedule.conflictArcs[arcIndex];
            if (arc.preferredWeight == 0) continue;
            int partnerAssignment = -1;
            int partnerUnit = -1;
            if (!partnerOf(schedule, arc, meeting, partnerAssignment, partnerUnit)) continue;
            int partnerStart = 0;
            int partnerEnd = 0;
            detail::spanOf(schedule, partnerUnit, partnerAssignment, partnerStart, partnerEnd);
            total += arc.preferredWeight *
                     ScoreCalculator::overlapGrains(start, end, partnerStart, partnerEnd);
        }
        return total;
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_PREFERRED_ATTENDANCE_CONFLICT_COST_HPP

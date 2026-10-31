#ifndef MS_CONSTRAINTS_REQUIRED_AND_PREFERRED_ATTENDANCE_CONFLICT_COST_HPP
#define MS_CONSTRAINTS_REQUIRED_AND_PREFERRED_ATTENDANCE_CONFLICT_COST_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Medium, weight 1 (medium units): requiredAndPreferredAttendanceConflictCost
// — a person required at one meeting and merely preferred at another that
// overlaps it. Raw cost (global) = overlapping grains x such (required,
// preferred) attendee pairs, summed over meeting pairs in both directions
// (person-grains) — which is why the arc carries a forward and a reverse
// weight and this component sums them; 0 = no such clash.
//
// The domain folds the medium level into the engine's soft level as
// `medium * kMediumScale` (kMediumFold in domain.hpp); the fold lives in the
// calculator, not in this weight. hard() gives 1 hard per person-grain,
// soft() 1 soft per person-grain.
struct RequiredAndPreferredAttendanceConflictCost final
    : AttendanceArcConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Medium;

    explicit RequiredAndPreferredAttendanceConflictCost(double weight
                                                        = kDefaultWeight)
        : AttendanceArcConstraint("requiredAndPreferredAttendanceConflictCost",
                                  weight,
                                  kDefaultLevel)
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

#endif  // MS_CONSTRAINTS_REQUIRED_AND_PREFERRED_ATTENDANCE_CONFLICT_COST_HPP

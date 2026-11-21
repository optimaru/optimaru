#ifndef MS_CONSTRAINTS_MEETING_OVERLAP_COST_HPP
#define MS_CONSTRAINTS_MEETING_OVERLAP_COST_HPP

#include "ms/constraints/common.hpp"

namespace ms {

// Soft, weight 10: meetingOverlapCost — ANY two meetings running at the
// same time, room and attendees irrelevant. It spreads the calendar out so
// people are not forced to choose between simultaneous meetings they might
// both want to join. Raw cost (global) = the overlapping grains of every pair
// of meetings, summed; 0 = no two meetings overlap.
struct MeetingOverlapCost final : AssignmentPairConstraint {
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit MeetingOverlapCost(double weight = kDefaultWeight)
        : AssignmentPairConstraint("meetingOverlapCost", weight, kDefaultLevel) {}

protected:
    std::int64_t marginal(const MeetingSchedule& schedule, int assignmentIndex,
                          int unit) const override {
        int start = 0;
        int end = 0;
        detail::spanOf(schedule, unit, assignmentIndex, start, end);
        std::int64_t grains = 0;
        const int count = static_cast<int>(schedule.meetingAssignments.size());
        for (int other = 0; other < count; ++other) {
            const int otherUnit = placedUnit(other);
            if (other == assignmentIndex || otherUnit < 0) continue;
            int otherStart = 0;
            int otherEnd = 0;
            detail::spanOf(schedule, otherUnit, other, otherStart, otherEnd);
            grains += ScoreCalculator::overlapGrains(start, end, otherStart, otherEnd);
        }
        return grains;
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_MEETING_OVERLAP_COST_HPP

#ifndef MS_CONSTRAINTS_OVERLAPPING_MEETINGS_HPP
#define MS_CONSTRAINTS_OVERLAPPING_MEETINGS_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Soft (default, weight 10): overlappingMeetings — one point per grain ANY
// two meetings run at the same time, room and attendees irrelevant. It spreads
// the calendar out so people are not forced to choose between simultaneous
// meetings they might both want to join.
struct OverlappingMeetings final : AssignmentPairConstraint
{
    explicit OverlappingMeetings(double weight = 10.0)
        : AssignmentPairConstraint(
              "overlappingMeetings", weight, /*hard=*/false)
    {
    }

protected:
    std::int64_t marginal(const MeetingSchedule &schedule,
                          int assignmentIndex,
                          int unit) const override
    {
        int start = 0;
        int end = 0;
        detail::spanOf(schedule, unit, assignmentIndex, start, end);
        std::int64_t grains = 0;
        const int count = static_cast<int>(schedule.meetingAssignments.size());
        for (int other = 0; other < count; ++other)
        {
            const int otherUnit = placedUnit(other);
            if (other == assignmentIndex || otherUnit < 0)
                continue;
            int otherStart = 0;
            int otherEnd = 0;
            detail::spanOf(schedule, otherUnit, other, otherStart, otherEnd);
            grains += ScoreCalculator::overlapGrains(
                start, end, otherStart, otherEnd);
        }
        return grains;
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_OVERLAPPING_MEETINGS_HPP

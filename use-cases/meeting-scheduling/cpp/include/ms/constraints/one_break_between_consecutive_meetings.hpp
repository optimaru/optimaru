#ifndef MS_CONSTRAINTS_ONE_BREAK_BETWEEN_CONSECUTIVE_MEETINGS_HPP
#define MS_CONSTRAINTS_ONE_BREAK_BETWEEN_CONSECUTIVE_MEETINGS_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Soft (default, weight 100): oneBreakBetweenConsecutiveMeetings — two
// meetings scheduled back to back with no grain in between. The most
// expensive soft term of the provider: a day of meetings with no gaps is the
// thing attendees complain about.
struct OneBreakBetweenConsecutiveMeetings final : AssignmentPairConstraint
{
    explicit OneBreakBetweenConsecutiveMeetings(double weight = 100.0)
        : AssignmentPairConstraint("oneBreakBetweenConsecutiveMeetings",
                                   weight,
                                   /*hard=*/false)
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
        std::int64_t adjacencies = 0;
        const int count = static_cast<int>(schedule.meetingAssignments.size());
        for (int other = 0; other < count; ++other)
        {
            const int otherUnit = placedUnit(other);
            if (other == assignmentIndex || otherUnit < 0)
                continue;
            int otherStart = 0;
            int otherEnd = 0;
            detail::spanOf(schedule, otherUnit, other, otherStart, otherEnd);
            // Both directions; at most one can match.
            if (end == otherStart)
                ++adjacencies;
            if (otherEnd == start)
                ++adjacencies;
        }
        return adjacencies;
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_ONE_BREAK_BETWEEN_CONSECUTIVE_MEETINGS_HPP

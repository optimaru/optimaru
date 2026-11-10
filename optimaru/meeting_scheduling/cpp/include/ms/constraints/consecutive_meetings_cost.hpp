#ifndef MS_CONSTRAINTS_CONSECUTIVE_MEETINGS_COST_HPP
#define MS_CONSTRAINTS_CONSECUTIVE_MEETINGS_COST_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Soft, weight 100: consecutiveMeetingsCost — two meetings scheduled back to
// back with no grain in between, whatever their rooms and attendees. The most
// expensive soft term: a day of meetings with no gaps is the
// thing attendees complain about. Raw cost (global) = the number of meeting
// pairs where one ends at the grain the other starts; 0 = every meeting is
// followed by a break.
struct ConsecutiveMeetingsCost final : AssignmentPairConstraint
{
    static constexpr double kDefaultWeight = 100.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit ConsecutiveMeetingsCost(double weight = kDefaultWeight)
        : AssignmentPairConstraint("consecutiveMeetingsCost",
                                   weight,
                                   kDefaultLevel)
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

#endif  // MS_CONSTRAINTS_CONSECUTIVE_MEETINGS_COST_HPP

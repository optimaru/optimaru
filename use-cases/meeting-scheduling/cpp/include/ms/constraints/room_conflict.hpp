#ifndef MS_CONSTRAINTS_ROOM_CONFLICT_HPP
#define MS_CONSTRAINTS_ROOM_CONFLICT_HPP

#include "ms/constraints/common.hpp"

namespace ms
{

// Hard (default, weight 1): roomConflict — one point per grain two meetings
// occupy the same room at the same time.
struct RoomConflict final : AssignmentPairConstraint
{
    explicit RoomConflict(double weight = 1.0)
        : AssignmentPairConstraint("roomConflict", weight, /*hard=*/true)
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
        const int room = schedule.roomIndexOf(unit);
        std::int64_t grains = 0;
        const int count = static_cast<int>(schedule.meetingAssignments.size());
        for (int other = 0; other < count; ++other)
        {
            const int otherUnit = placedUnit(other);
            if (other == assignmentIndex || otherUnit < 0)
                continue;
            if (schedule.roomIndexOf(otherUnit) != room)
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

#endif  // MS_CONSTRAINTS_ROOM_CONFLICT_HPP

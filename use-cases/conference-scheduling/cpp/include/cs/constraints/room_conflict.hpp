#ifndef CS_CONSTRAINTS_ROOM_CONFLICT_HPP
#define CS_CONSTRAINTS_ROOM_CONFLICT_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 1000): roomConflict — two talks sharing a room while
// their timeslots overlap, weighed by the overlapping minutes.
struct RoomConflict final : BreakdownConstraint
{
    explicit RoomConflict(double weight = 1000.0)
        : BreakdownConstraint("roomConflict", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.roomConflict;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_ROOM_CONFLICT_HPP

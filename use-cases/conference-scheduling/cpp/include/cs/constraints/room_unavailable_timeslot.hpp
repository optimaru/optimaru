#ifndef CS_CONSTRAINTS_ROOM_UNAVAILABLE_TIMESLOT_HPP
#define CS_CONSTRAINTS_ROOM_UNAVAILABLE_TIMESLOT_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 100000): roomUnavailableTimeslot — a talk in a room
// that is closed during its timeslot.
struct RoomUnavailableTimeslot final : BreakdownConstraint
{
    explicit RoomUnavailableTimeslot(double weight = 100000.0)
        : BreakdownConstraint("roomUnavailableTimeslot", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.roomUnavailableTimeslot;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_ROOM_UNAVAILABLE_TIMESLOT_HPP

#ifndef CS_CONSTRAINTS_TALK_REQUIRED_ROOM_TAGS_COST_HPP
#define CS_CONSTRAINTS_TALK_REQUIRED_ROOM_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 1: talkRequiredRoomTagsCost — a room tag the talk requires and
// the assigned room lacks.
// Raw cost (one global total) = the sum of required tags the room lacks x the
// timeslot's duration, per talk; 0 = no violation (every talk's required room
// tags are present).
struct TalkRequiredRoomTagsCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TalkRequiredRoomTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint("talkRequiredRoomTagsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkRequiredRoomTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_REQUIRED_ROOM_TAGS_COST_HPP

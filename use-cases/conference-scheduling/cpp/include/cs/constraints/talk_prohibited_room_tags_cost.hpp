#ifndef CS_CONSTRAINTS_TALK_PROHIBITED_ROOM_TAGS_COST_HPP
#define CS_CONSTRAINTS_TALK_PROHIBITED_ROOM_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 1: talkProhibitedRoomTagsCost — a room tag the talk forbids and
// the assigned room carries.
// Raw cost (one global total) = the sum of prohibited tags the room carries x
// the timeslot's duration, per talk; 0 = no violation (no talk sits in a room
// with a tag it prohibits).
struct TalkProhibitedRoomTagsCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TalkProhibitedRoomTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "talkProhibitedRoomTagsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkProhibitedRoomTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_PROHIBITED_ROOM_TAGS_COST_HPP

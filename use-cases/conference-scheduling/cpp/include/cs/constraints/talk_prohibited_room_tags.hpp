#ifndef CS_CONSTRAINTS_TALK_PROHIBITED_ROOM_TAGS_HPP
#define CS_CONSTRAINTS_TALK_PROHIBITED_ROOM_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 1): talkProhibitedRoomTags — a room tag the talk
// forbids and the assigned room carries.
struct TalkProhibitedRoomTags final : BreakdownConstraint
{
    explicit TalkProhibitedRoomTags(double weight = 1.0)
        : BreakdownConstraint("talkProhibitedRoomTags", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkProhibitedRoomTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_PROHIBITED_ROOM_TAGS_HPP

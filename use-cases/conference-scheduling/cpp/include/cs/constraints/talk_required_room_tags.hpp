#ifndef CS_CONSTRAINTS_TALK_REQUIRED_ROOM_TAGS_HPP
#define CS_CONSTRAINTS_TALK_REQUIRED_ROOM_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 1): talkRequiredRoomTags — a room tag the talk requires
// and the assigned room lacks.
struct TalkRequiredRoomTags final : BreakdownConstraint
{
    explicit TalkRequiredRoomTags(double weight = 1.0)
        : BreakdownConstraint("talkRequiredRoomTags", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkRequiredRoomTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_REQUIRED_ROOM_TAGS_HPP

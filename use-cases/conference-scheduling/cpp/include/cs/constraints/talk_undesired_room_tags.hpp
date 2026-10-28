#ifndef CS_CONSTRAINTS_TALK_UNDESIRED_ROOM_TAGS_HPP
#define CS_CONSTRAINTS_TALK_UNDESIRED_ROOM_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 20): talkUndesiredRoomTags — a room tag the talk would
// rather avoid and the assigned room carries.
struct TalkUndesiredRoomTags final : BreakdownConstraint
{
    explicit TalkUndesiredRoomTags(double weight = 20.0)
        : BreakdownConstraint("talkUndesiredRoomTags", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkUndesiredRoomTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_UNDESIRED_ROOM_TAGS_HPP

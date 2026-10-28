#ifndef CS_CONSTRAINTS_TALK_PREFERRED_ROOM_TAGS_HPP
#define CS_CONSTRAINTS_TALK_PREFERRED_ROOM_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 20): talkPreferredRoomTags — a room tag the talk
// prefers and the assigned room lacks.
struct TalkPreferredRoomTags final : BreakdownConstraint
{
    explicit TalkPreferredRoomTags(double weight = 20.0)
        : BreakdownConstraint("talkPreferredRoomTags", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkPreferredRoomTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_PREFERRED_ROOM_TAGS_HPP

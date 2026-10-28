#ifndef CS_CONSTRAINTS_SPEAKER_PROHIBITED_ROOM_TAGS_HPP
#define CS_CONSTRAINTS_SPEAKER_PROHIBITED_ROOM_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 1): speakerProhibitedRoomTags — a room tag a speaker
// forbids and the assigned room carries.
struct SpeakerProhibitedRoomTags final : BreakdownConstraint
{
    explicit SpeakerProhibitedRoomTags(double weight = 1.0)
        : BreakdownConstraint(
              "speakerProhibitedRoomTags", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerProhibitedRoomTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_PROHIBITED_ROOM_TAGS_HPP

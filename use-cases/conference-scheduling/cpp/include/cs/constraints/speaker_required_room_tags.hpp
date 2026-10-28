#ifndef CS_CONSTRAINTS_SPEAKER_REQUIRED_ROOM_TAGS_HPP
#define CS_CONSTRAINTS_SPEAKER_REQUIRED_ROOM_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 1): speakerRequiredRoomTags — a room tag a speaker
// requires and the assigned room lacks.
struct SpeakerRequiredRoomTags final : BreakdownConstraint
{
    explicit SpeakerRequiredRoomTags(double weight = 1.0)
        : BreakdownConstraint("speakerRequiredRoomTags", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerRequiredRoomTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_REQUIRED_ROOM_TAGS_HPP

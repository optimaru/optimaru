#ifndef CS_CONSTRAINTS_SPEAKER_PREFERRED_ROOM_TAGS_HPP
#define CS_CONSTRAINTS_SPEAKER_PREFERRED_ROOM_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 20): speakerPreferredRoomTags — a room tag a speaker
// prefers and the assigned room lacks.
struct SpeakerPreferredRoomTags final : BreakdownConstraint
{
    explicit SpeakerPreferredRoomTags(double weight = 20.0)
        : BreakdownConstraint(
              "speakerPreferredRoomTags", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerPreferredRoomTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_PREFERRED_ROOM_TAGS_HPP

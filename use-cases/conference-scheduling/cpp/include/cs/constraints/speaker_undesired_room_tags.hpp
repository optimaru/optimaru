#ifndef CS_CONSTRAINTS_SPEAKER_UNDESIRED_ROOM_TAGS_HPP
#define CS_CONSTRAINTS_SPEAKER_UNDESIRED_ROOM_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 20): speakerUndesiredRoomTags — a room tag a speaker
// would rather avoid and the assigned room carries.
struct SpeakerUndesiredRoomTags final : BreakdownConstraint
{
    explicit SpeakerUndesiredRoomTags(double weight = 20.0)
        : BreakdownConstraint(
              "speakerUndesiredRoomTags", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerUndesiredRoomTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_UNDESIRED_ROOM_TAGS_HPP

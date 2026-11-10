#ifndef CS_CONSTRAINTS_SPEAKER_UNDESIRED_ROOM_TAGS_COST_HPP
#define CS_CONSTRAINTS_SPEAKER_UNDESIRED_ROOM_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft, weight 20: speakerUndesiredRoomTagsCost — a room tag a speaker would
// rather avoid and the assigned room carries.
// Raw cost (one global total) = the sum of undesired tags the room carries x
// the timeslot's duration, per speaker of each talk; 0 = no violation (no
// speaker sits in a room with a tag they would rather avoid).
struct SpeakerUndesiredRoomTagsCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 20.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit SpeakerUndesiredRoomTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "speakerUndesiredRoomTagsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerUndesiredRoomTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_UNDESIRED_ROOM_TAGS_COST_HPP

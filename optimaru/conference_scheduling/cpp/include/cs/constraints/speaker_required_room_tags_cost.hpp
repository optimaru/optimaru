#ifndef CS_CONSTRAINTS_SPEAKER_REQUIRED_ROOM_TAGS_COST_HPP
#define CS_CONSTRAINTS_SPEAKER_REQUIRED_ROOM_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 1: speakerRequiredRoomTagsCost — a room tag a speaker requires
// and the assigned room lacks.
// Raw cost (one global total) = the sum of required tags the room lacks x the
// timeslot's duration, per speaker of each talk; 0 = no violation (every
// speaker's required room tags are present).
struct SpeakerRequiredRoomTagsCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit SpeakerRequiredRoomTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "speakerRequiredRoomTagsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerRequiredRoomTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_REQUIRED_ROOM_TAGS_COST_HPP

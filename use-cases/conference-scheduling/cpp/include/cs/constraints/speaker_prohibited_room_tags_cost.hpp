#ifndef CS_CONSTRAINTS_SPEAKER_PROHIBITED_ROOM_TAGS_COST_HPP
#define CS_CONSTRAINTS_SPEAKER_PROHIBITED_ROOM_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 1: speakerProhibitedRoomTagsCost — a room tag a speaker forbids
// and the assigned room carries.
// Raw cost (one global total) = the sum of prohibited tags the room carries x
// the timeslot's duration, per speaker of each talk; 0 = no violation (no
// speaker sits in a room with a tag they prohibit).
struct SpeakerProhibitedRoomTagsCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit SpeakerProhibitedRoomTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "speakerProhibitedRoomTagsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerProhibitedRoomTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_PROHIBITED_ROOM_TAGS_COST_HPP

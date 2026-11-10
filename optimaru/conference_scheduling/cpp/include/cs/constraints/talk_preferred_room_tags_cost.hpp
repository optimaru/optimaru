#ifndef CS_CONSTRAINTS_TALK_PREFERRED_ROOM_TAGS_COST_HPP
#define CS_CONSTRAINTS_TALK_PREFERRED_ROOM_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft, weight 20: talkPreferredRoomTagsCost — a room tag the talk prefers and
// the assigned room lacks.
// Raw cost (one global total) = the sum of preferred tags the room lacks x the
// timeslot's duration, per talk; 0 = no violation (every talk's preferred room
// tags are present).
struct TalkPreferredRoomTagsCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 20.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit TalkPreferredRoomTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "talkPreferredRoomTagsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkPreferredRoomTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_PREFERRED_ROOM_TAGS_COST_HPP

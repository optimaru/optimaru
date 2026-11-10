#ifndef CS_CONSTRAINTS_TALK_UNDESIRED_ROOM_TAGS_COST_HPP
#define CS_CONSTRAINTS_TALK_UNDESIRED_ROOM_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Soft, weight 20: talkUndesiredRoomTagsCost — a room tag the talk would
// rather avoid and the assigned room carries.
// Raw cost (one global total) = the sum of undesired tags the room carries x
// the timeslot's duration, per talk; 0 = no violation (no talk sits in a room
// with a tag it would rather avoid).
struct TalkUndesiredRoomTagsCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 20.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit TalkUndesiredRoomTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint("talkUndesiredRoomTagsCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.talkUndesiredRoomTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_UNDESIRED_ROOM_TAGS_COST_HPP

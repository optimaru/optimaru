#ifndef CS_CONSTRAINTS_ROOM_UNAVAILABILITY_COST_HPP
#define CS_CONSTRAINTS_ROOM_UNAVAILABILITY_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Hard, weight 100000: roomUnavailabilityCost — a talk in a room that is
// closed during its timeslot.
// Raw cost (one global total) = the sum of the timeslot's duration in minutes
// for each talk in a room that is unavailable then; 0 = no violation (no talk
// sits in an unavailable room).
struct RoomUnavailabilityCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 100000.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RoomUnavailabilityCost(double weight = kDefaultWeight)
        : BreakdownConstraint("roomUnavailabilityCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.roomUnavailabilityCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_ROOM_UNAVAILABILITY_COST_HPP

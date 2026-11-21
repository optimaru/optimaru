#ifndef CS_CONSTRAINTS_ROOM_CONFLICT_COST_HPP
#define CS_CONSTRAINTS_ROOM_CONFLICT_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Hard, weight 1000: roomConflictCost — two talks sharing a room while their
// timeslots overlap, weighed by the overlapping minutes.
// Raw cost (one global total) = the sum of overlapping minutes for each pair
// of talks in the same room; 0 = no violation (no room is double-booked).
struct RoomConflictCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 1000.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RoomConflictCost(double weight = kDefaultWeight)
        : BreakdownConstraint("roomConflictCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.roomConflictCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_ROOM_CONFLICT_COST_HPP

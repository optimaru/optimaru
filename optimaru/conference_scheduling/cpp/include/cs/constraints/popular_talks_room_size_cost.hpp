#ifndef CS_CONSTRAINTS_POPULAR_TALKS_ROOM_SIZE_COST_HPP
#define CS_CONSTRAINTS_POPULAR_TALKS_ROOM_SIZE_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Soft, weight 10: popularTalksRoomSizeCost — a less popular talk given a
// bigger room than a more popular one.
// Raw cost (one global total) = the sum of the combined duration for each pair
// where the talk with fewer favorites has the larger room; 0 = no violation
// (room sizes follow popularity).
struct PopularTalksRoomSizeCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit PopularTalksRoomSizeCost(double weight = kDefaultWeight)
        : BreakdownConstraint("popularTalksRoomSizeCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.popularTalksRoomSizeCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_POPULAR_TALKS_ROOM_SIZE_COST_HPP

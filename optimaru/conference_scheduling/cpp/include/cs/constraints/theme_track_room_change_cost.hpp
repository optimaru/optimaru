#ifndef CS_CONSTRAINTS_THEME_TRACK_ROOM_CHANGE_COST_HPP
#define CS_CONSTRAINTS_THEME_TRACK_ROOM_CHANGE_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Soft, weight 10: themeTrackRoomChangeCost — two talks of one theme track on
// the same day but in different rooms, so the audience following that track
// has to move.
// Raw cost (one global total) = the sum of shared theme tracks x the combined
// duration for each same-day pair of talks in different rooms; 0 = no
// violation (every theme track keeps one room per day).
struct ThemeTrackRoomChangeCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit ThemeTrackRoomChangeCost(double weight = kDefaultWeight)
        : BreakdownConstraint("themeTrackRoomChangeCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.themeTrackRoomChangeCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_THEME_TRACK_ROOM_CHANGE_COST_HPP

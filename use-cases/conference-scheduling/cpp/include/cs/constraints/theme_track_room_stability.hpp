#ifndef CS_CONSTRAINTS_THEME_TRACK_ROOM_STABILITY_HPP
#define CS_CONSTRAINTS_THEME_TRACK_ROOM_STABILITY_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 10): themeTrackRoomStability — two talks of one theme
// track on the same day but in different rooms, so the audience following that
// track has to move.
struct ThemeTrackRoomStability final : BreakdownConstraint
{
    explicit ThemeTrackRoomStability(double weight = 10.0)
        : BreakdownConstraint("themeTrackRoomStability", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.themeTrackRoomStability;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_THEME_TRACK_ROOM_STABILITY_HPP

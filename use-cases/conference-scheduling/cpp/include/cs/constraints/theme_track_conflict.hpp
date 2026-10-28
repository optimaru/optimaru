#ifndef CS_CONSTRAINTS_THEME_TRACK_CONFLICT_HPP
#define CS_CONSTRAINTS_THEME_TRACK_CONFLICT_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 10): themeTrackConflict — two overlapping talks on the
// same theme track, weighed by shared themes times the overlap: an attendee
// following a theme cannot split in two.
struct ThemeTrackConflict final : BreakdownConstraint
{
    explicit ThemeTrackConflict(double weight = 10.0)
        : BreakdownConstraint("themeTrackConflict", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.themeTrackConflict;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_THEME_TRACK_CONFLICT_HPP

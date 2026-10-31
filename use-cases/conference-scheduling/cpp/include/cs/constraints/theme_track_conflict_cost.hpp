#ifndef CS_CONSTRAINTS_THEME_TRACK_CONFLICT_COST_HPP
#define CS_CONSTRAINTS_THEME_TRACK_CONFLICT_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft, weight 10: themeTrackConflictCost — two overlapping talks on the same
// theme track, weighed by shared themes times the overlap: an attendee
// following a theme cannot split in two.
// Raw cost (one global total) = the sum of shared theme tracks x overlapping
// minutes for each pair of overlapping talks; 0 = no violation (no overlapping
// talks share a theme track).
struct ThemeTrackConflictCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit ThemeTrackConflictCost(double weight = kDefaultWeight)
        : BreakdownConstraint("themeTrackConflictCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.themeTrackConflictCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_THEME_TRACK_CONFLICT_COST_HPP

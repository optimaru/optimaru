#ifndef CS_CONSTRAINTS_AUDIENCE_TYPE_THEME_TRACK_CONFLICT_COST_HPP
#define CS_CONSTRAINTS_AUDIENCE_TYPE_THEME_TRACK_CONFLICT_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft, weight 1: audienceTypeThemeTrackConflictCost — two overlapping talks
// sharing both a theme track and an audience type.
// Raw cost (one global total) = the sum of shared theme tracks x shared
// audience types x overlapping minutes for each pair of overlapping talks; 0 =
// no violation (no overlapping talks share both a theme track and an audience
// type).
struct AudienceTypeThemeTrackConflictCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit AudienceTypeThemeTrackConflictCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "audienceTypeThemeTrackConflictCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.audienceTypeThemeTrackConflictCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_AUDIENCE_TYPE_THEME_TRACK_CONFLICT_COST_HPP

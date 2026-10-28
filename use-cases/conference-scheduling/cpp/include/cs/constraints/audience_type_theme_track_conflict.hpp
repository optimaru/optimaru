#ifndef CS_CONSTRAINTS_AUDIENCE_TYPE_THEME_TRACK_CONFLICT_HPP
#define CS_CONSTRAINTS_AUDIENCE_TYPE_THEME_TRACK_CONFLICT_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 1): audienceTypeThemeTrackConflict — two overlapping
// talks sharing both a theme track and an audience type.
struct AudienceTypeThemeTrackConflict final : BreakdownConstraint
{
    explicit AudienceTypeThemeTrackConflict(double weight = 1.0)
        : BreakdownConstraint(
              "audienceTypeThemeTrackConflict", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.audienceTypeThemeTrackConflict;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_AUDIENCE_TYPE_THEME_TRACK_CONFLICT_HPP

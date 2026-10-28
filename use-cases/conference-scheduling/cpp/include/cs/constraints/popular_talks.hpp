#ifndef CS_CONSTRAINTS_POPULAR_TALKS_HPP
#define CS_CONSTRAINTS_POPULAR_TALKS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 10): popularTalks — a less popular talk given a bigger
// room than a more popular one.
struct PopularTalks final : BreakdownConstraint
{
    explicit PopularTalks(double weight = 10.0)
        : BreakdownConstraint("popularTalks", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.popularTalks;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_POPULAR_TALKS_HPP

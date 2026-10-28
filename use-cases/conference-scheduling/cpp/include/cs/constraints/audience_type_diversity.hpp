#ifndef CS_CONSTRAINTS_AUDIENCE_TYPE_DIVERSITY_HPP
#define CS_CONSTRAINTS_AUDIENCE_TYPE_DIVERSITY_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 1): audienceTypeDiversity — REWARD: one timeslot
// serving several audience types at once, weighed by audiences times the slot
// duration. Parallel tracks for different audiences are exactly what a
// conference wants.
struct AudienceTypeDiversity final : BreakdownConstraint
{
    explicit AudienceTypeDiversity(double weight = 1.0)
        : BreakdownConstraint("audienceTypeDiversity", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return -breakdown.audienceTypeDiversity;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_AUDIENCE_TYPE_DIVERSITY_HPP

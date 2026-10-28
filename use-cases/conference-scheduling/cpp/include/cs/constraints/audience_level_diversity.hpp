#ifndef CS_CONSTRAINTS_AUDIENCE_LEVEL_DIVERSITY_HPP
#define CS_CONSTRAINTS_AUDIENCE_LEVEL_DIVERSITY_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 1): audienceLevelDiversity — REWARD: one timeslot
// mixing audience levels, so beginners and experts both have somewhere to be.
struct AudienceLevelDiversity final : BreakdownConstraint
{
    explicit AudienceLevelDiversity(double weight = 1.0)
        : BreakdownConstraint("audienceLevelDiversity", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return -breakdown.audienceLevelDiversity;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_AUDIENCE_LEVEL_DIVERSITY_HPP

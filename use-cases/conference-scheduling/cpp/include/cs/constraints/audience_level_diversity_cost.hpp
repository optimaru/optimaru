#ifndef CS_CONSTRAINTS_AUDIENCE_LEVEL_DIVERSITY_COST_HPP
#define CS_CONSTRAINTS_AUDIENCE_LEVEL_DIVERSITY_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft, weight 1, REWARD (raw cost <= 0; never hard, not flippable):
// audienceLevelDiversityCost — one timeslot mixing audience levels, so
// beginners and experts both have somewhere to be.
// Raw cost (one global total) = the sum of -(the timeslot's duration) for each
// pair of talks in the same timeslot with different audience levels; 0 = no
// reward earned (no timeslot mixes audience levels).
struct AudienceLevelDiversityCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit AudienceLevelDiversityCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "audienceLevelDiversityCost", weight, kDefaultLevel)
    {
        reward = true;  // raw cost <= 0; level='hard' is refused
        levelFlippable = false;
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return -breakdown.audienceLevelDiversityCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_AUDIENCE_LEVEL_DIVERSITY_COST_HPP

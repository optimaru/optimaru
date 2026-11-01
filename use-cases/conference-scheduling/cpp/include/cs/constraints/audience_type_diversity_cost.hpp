#ifndef CS_CONSTRAINTS_AUDIENCE_TYPE_DIVERSITY_COST_HPP
#define CS_CONSTRAINTS_AUDIENCE_TYPE_DIVERSITY_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft, weight 1, REWARD (raw cost <= 0; never hard, not flippable):
// audienceTypeDiversityCost — one timeslot serving audiences at once, weighed
// by audience types times the slot duration. The arithmetic is kept from 0.1.x
// on purpose (0.2.0 renames only): it rewards same-timeslot pairs that SHARE
// an audience type, not pairs of different audiences, despite the name.
// Raw cost (one global total) = the sum of -(shared audience types x the
// timeslot's duration) for each pair of talks in the same timeslot; 0 = no
// reward earned (no same-timeslot pair shares an audience type).
struct AudienceTypeDiversityCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit AudienceTypeDiversityCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "audienceTypeDiversityCost", weight, kDefaultLevel)
    {
        reward = true;  // raw cost <= 0; level='hard' is refused
        levelFlippable = false;
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return -breakdown.audienceTypeDiversityCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_AUDIENCE_TYPE_DIVERSITY_COST_HPP

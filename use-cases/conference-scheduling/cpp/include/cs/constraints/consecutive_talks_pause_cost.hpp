#ifndef CS_CONSTRAINTS_CONSECUTIVE_TALKS_PAUSE_COST_HPP
#define CS_CONSTRAINTS_CONSECUTIVE_TALKS_PAUSE_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 1: consecutiveTalksPauseCost — two talks of one speaker
// scheduled back to back without the minimum pause between them, weighed by
// their combined duration.
// Raw cost (one global total) = the sum of the combined duration of both talks
// for each pair sharing a speaker without the minimum pause between them; 0 =
// no violation (every speaker gets the minimum pause).
struct ConsecutiveTalksPauseCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ConsecutiveTalksPauseCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "consecutiveTalksPauseCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.consecutiveTalksPauseCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_CONSECUTIVE_TALKS_PAUSE_COST_HPP

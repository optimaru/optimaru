#ifndef CS_CONSTRAINTS_DIFFERENT_DAY_TALKS_COST_HPP
#define CS_CONSTRAINTS_DIFFERENT_DAY_TALKS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft, weight 10: differentDayTalksCost — two talks sharing content or a
// theme track scheduled on DIFFERENT days, which forces an attendee to come
// back for the second one.
// Raw cost (one global total) = the sum of (shared content tags + shared theme
// tracks) x the combined duration for each related pair on different days; 0 =
// no violation (related talks are on the same day).
struct DifferentDayTalksCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit DifferentDayTalksCost(double weight = kDefaultWeight)
        : BreakdownConstraint("differentDayTalksCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.differentDayTalksCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_DIFFERENT_DAY_TALKS_COST_HPP

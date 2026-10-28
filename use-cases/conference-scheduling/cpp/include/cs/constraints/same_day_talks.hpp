#ifndef CS_CONSTRAINTS_SAME_DAY_TALKS_HPP
#define CS_CONSTRAINTS_SAME_DAY_TALKS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 10): sameDayTalks — two talks sharing content or a
// theme track scheduled on DIFFERENT days, which forces an attendee to come
// back for the second one.
struct SameDayTalks final : BreakdownConstraint
{
    explicit SameDayTalks(double weight = 10.0)
        : BreakdownConstraint("sameDayTalks", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.sameDayTalks;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SAME_DAY_TALKS_HPP

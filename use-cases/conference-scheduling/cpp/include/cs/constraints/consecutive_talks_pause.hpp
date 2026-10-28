#ifndef CS_CONSTRAINTS_CONSECUTIVE_TALKS_PAUSE_HPP
#define CS_CONSTRAINTS_CONSECUTIVE_TALKS_PAUSE_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 1): consecutiveTalksPause — two talks of one speaker
// scheduled back to back without the minimum pause between them, weighed by
// their combined duration.
struct ConsecutiveTalksPause final : BreakdownConstraint
{
    explicit ConsecutiveTalksPause(double weight = 1.0)
        : BreakdownConstraint("consecutiveTalksPause", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.consecutiveTalksPause;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_CONSECUTIVE_TALKS_PAUSE_HPP

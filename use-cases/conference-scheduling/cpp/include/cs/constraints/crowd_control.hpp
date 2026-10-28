#ifndef CS_CONSTRAINTS_CROWD_CONTROL_HPP
#define CS_CONSTRAINTS_CROWD_CONTROL_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 1): crowdControl — a talk with a crowd-control risk
// sharing its timeslot with two or more other risky talks — the shape that
// overwhelms the corridors.
struct CrowdControl final : BreakdownConstraint
{
    explicit CrowdControl(double weight = 1.0)
        : BreakdownConstraint("crowdControl", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.crowdControl;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_CROWD_CONTROL_HPP

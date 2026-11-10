#ifndef CS_CONSTRAINTS_CROWD_CONTROL_RISK_COST_HPP
#define CS_CONSTRAINTS_CROWD_CONTROL_RISK_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 1: crowdControlRiskCost — a talk with a crowd-control risk
// sharing its timeslot with two or more other risky talks — the shape that
// overwhelms the corridors.
// Raw cost (one global total) = the sum of the timeslot's duration for each
// risky talk (crowd_control_risk > 0) in a timeslot holding three or more
// risky talks; 0 = no violation (no timeslot holds three or more risky talks).
struct CrowdControlRiskCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit CrowdControlRiskCost(double weight = kDefaultWeight)
        : BreakdownConstraint("crowdControlRiskCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.crowdControlRiskCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_CROWD_CONTROL_RISK_COST_HPP

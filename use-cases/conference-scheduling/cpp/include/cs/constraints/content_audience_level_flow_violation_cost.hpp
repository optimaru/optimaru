#ifndef CS_CONSTRAINTS_CONTENT_AUDIENCE_LEVEL_FLOW_VIOLATION_COST_HPP
#define CS_CONSTRAINTS_CONTENT_AUDIENCE_LEVEL_FLOW_VIOLATION_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft, weight 10: contentAudienceLevelFlowViolationCost — a beginner talk
// scheduled after (or overlapping) an advanced talk on the same content, so
// the audience cannot follow the level progression.
// Raw cost (one global total) = the sum of shared content tags x the combined
// duration for each pair where the lower-level talk does not end before the
// higher-level one starts; 0 = no violation (every content goes from the lower
// audience level to the higher one).
struct ContentAudienceLevelFlowViolationCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit ContentAudienceLevelFlowViolationCost(double weight
                                                   = kDefaultWeight)
        : BreakdownConstraint(
              "contentAudienceLevelFlowViolationCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.contentAudienceLevelFlowViolationCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_CONTENT_AUDIENCE_LEVEL_FLOW_VIOLATION_COST_HPP

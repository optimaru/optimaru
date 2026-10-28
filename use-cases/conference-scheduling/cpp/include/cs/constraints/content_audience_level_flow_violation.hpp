#ifndef CS_CONSTRAINTS_CONTENT_AUDIENCE_LEVEL_FLOW_VIOLATION_HPP
#define CS_CONSTRAINTS_CONTENT_AUDIENCE_LEVEL_FLOW_VIOLATION_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 10): contentAudienceLevelFlowViolation — a beginner
// talk scheduled after (or overlapping) an advanced talk on the same content,
// so the audience cannot follow the level progression.
struct ContentAudienceLevelFlowViolation final : BreakdownConstraint
{
    explicit ContentAudienceLevelFlowViolation(double weight = 10.0)
        : BreakdownConstraint(
              "contentAudienceLevelFlowViolation", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.contentAudienceLevelFlowViolation;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_CONTENT_AUDIENCE_LEVEL_FLOW_VIOLATION_HPP

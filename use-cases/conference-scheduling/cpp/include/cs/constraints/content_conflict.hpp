#ifndef CS_CONSTRAINTS_CONTENT_CONFLICT_HPP
#define CS_CONSTRAINTS_CONTENT_CONFLICT_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 100): contentConflict — two overlapping talks on the
// same content. The most expensive soft rule: the same audience wants both.
struct ContentConflict final : BreakdownConstraint
{
    explicit ContentConflict(double weight = 100.0)
        : BreakdownConstraint("contentConflict", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.contentConflict;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_CONTENT_CONFLICT_HPP

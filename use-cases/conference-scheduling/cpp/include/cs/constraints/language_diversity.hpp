#ifndef CS_CONSTRAINTS_LANGUAGE_DIVERSITY_HPP
#define CS_CONSTRAINTS_LANGUAGE_DIVERSITY_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 10): languageDiversity — REWARD: one timeslot offering
// talks in different languages.
struct LanguageDiversity final : BreakdownConstraint
{
    explicit LanguageDiversity(double weight = 10.0)
        : BreakdownConstraint("languageDiversity", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return -breakdown.languageDiversity;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_LANGUAGE_DIVERSITY_HPP

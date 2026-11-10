#ifndef CS_CONSTRAINTS_CONTENT_CONFLICT_COST_HPP
#define CS_CONSTRAINTS_CONTENT_CONFLICT_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Soft, weight 100: contentConflictCost — two overlapping talks on the same
// content. The most expensive soft rule: the same audience wants both.
// Raw cost (one global total) = the sum of shared content tags x overlapping
// minutes for each pair of overlapping talks; 0 = no violation (no overlapping
// talks share content).
struct ContentConflictCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 100.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit ContentConflictCost(double weight = kDefaultWeight)
        : BreakdownConstraint("contentConflictCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.contentConflictCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_CONTENT_CONFLICT_COST_HPP

#ifndef MC_CONSTRAINTS_PARTITION_SIZE_BOUNDS_COST_HPP
#define MC_CONSTRAINTS_PARTITION_SIZE_BOUNDS_COST_HPP

#include <algorithm>
#include <vector>

#include "mc/constraints/common.hpp"

namespace mc {

// Hard, weight 1: partitionSizeBoundsCost — the vertices a side has fewer
// than min_partition_size or more than max_partition_size. Raw cost per side
// u = max(0, min - n_u) + max(0, n_u - max) with n_u the side's size (min
// defaults to 0, max to |V|), so 0 = both sides within the bounds. The clamp is not additive per
// member, so there is no delta: a touched side is re-costed from scratch, which is O(1)
// (members.size()).
struct PartitionSizeBoundsCost final : McUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit PartitionSizeBoundsCost(double weight = kDefaultWeight)
        : McUnitConstraint("partitionSizeBoundsCost", weight, kDefaultLevel) {}

    void on_reset(const MaxCutProblem& problem) override { detail::requirePrepared(problem); }

    double cost(const MaxCutProblem& problem, int /*unit*/,
                const std::vector<int>& members) const override {
        const int n = static_cast<int>(members.size());
        return static_cast<double>(std::max(0, problem.minSize() - n) +
                                   std::max(0, n - problem.max_size()));
    }
};

}  // namespace mc

#endif  // MC_CONSTRAINTS_PARTITION_SIZE_BOUNDS_COST_HPP

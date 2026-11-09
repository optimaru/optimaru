#ifndef MC_CONSTRAINTS_PARTITION_IMBALANCE_COST_HPP
#define MC_CONSTRAINTS_PARTITION_IMBALANCE_COST_HPP

#include <cstdlib>
#include <vector>

#include "mc/constraints/common.hpp"

namespace mc
{

// Soft, weight 0 (opt-in): partitionImbalanceCost — how far the two sides are
// from equal size. Raw cost = |n0 - n1| (one total), so 0 = equal sides (or,
// with an odd vertex count, never 0: at best 1). The same one-term list as
// 0.1.x, so the exponent semantics are unchanged. Weight 0 keeps it out of
// the default score; pick it with a weight to balance the cut. Counts kept
// from the joins / leaves: O(1).
struct PartitionImbalanceCost final : detail::MemberChangeConstraint
{
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit PartitionImbalanceCost(double weight = kDefaultWeight)
        : detail::MemberChangeConstraint("partitionImbalanceCost", weight, kDefaultLevel)
    {
    }

    void reset(const MaxCutProblem &problem, const std::vector<std::vector<int>> &members_per_unit) override
    {
        detail::requirePrepared(problem);
        for (int s = 0; s < kSideCount; ++s)
            count_[s] = s < static_cast<int>(members_per_unit.size()) ? static_cast<int>(members_per_unit[s].size()) : 0;
    }

    void on_member_changed(const MaxCutProblem & /*problem*/,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int /*vertex*/,
                         bool joined) override
    {
        count_[unit] += joined ? 1 : -1;
    }

    double cost() const override { return static_cast<double>(std::abs(count_[0] - count_[1])); }

private:
    int count_[kSideCount] = {0, 0};
};

}  // namespace mc

#endif  // MC_CONSTRAINTS_PARTITION_IMBALANCE_COST_HPP

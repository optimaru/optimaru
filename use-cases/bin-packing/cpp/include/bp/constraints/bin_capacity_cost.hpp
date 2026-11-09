#ifndef BP_CONSTRAINTS_BIN_CAPACITY_COST_HPP
#define BP_CONSTRAINTS_BIN_CAPACITY_COST_HPP

#include "bp/constraints/common.hpp"

namespace bp
{

// Hard, weight 1: binCapacityCost — the load a bin carries beyond the
// capacity. Raw cost per bin = max(0, summed item weight - binCapacity), so
// 0 = within capacity (an empty bin costs 0).
//
// No delta contract: max(0, load - capacity) is not a sum of per-member
// terms (the clamp is what makes it non-additive), so the marginal cost of
// one item depends on the whole bin. cost() is O(members) either way, so the
// calculator's full re-cost path loses nothing.
struct BinCapacityCost final : BpUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit BinCapacityCost(double weight = kDefaultWeight)
        : BpUnitConstraint("binCapacityCost", weight, kDefaultLevel)
    {
    }
    void on_reset(const BinPackingProblem &problem) override
    {
        detail::requirePrepared(problem);
    }
    double cost(const BinPackingProblem &problem,
                int /*unit*/,
                const std::vector<int> &members) const override
    {
        std::int64_t load = 0;
        for (int item : members)
            load += problem.items[item].weight;
        const std::int64_t over = load - problem.binCapacity;
        return over > 0 ? static_cast<double>(over) : 0.0;
    }
};

}  // namespace bp

#endif  // BP_CONSTRAINTS_BIN_CAPACITY_COST_HPP

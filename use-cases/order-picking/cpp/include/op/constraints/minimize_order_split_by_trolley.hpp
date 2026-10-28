#ifndef OP_CONSTRAINTS_MINIMIZE_ORDER_SPLIT_BY_TROLLEY_HPP
#define OP_CONSTRAINTS_MINIMIZE_ORDER_SPLIT_BY_TROLLEY_HPP

#include "op/constraints/common.hpp"

namespace op
{

// Soft (default, weight 1'000): minimizeOrderSplitByTrolley — one point per
// (order, trolley) pairing, so an order carried by three trolleys costs three
// times as much as one carried by a single trolley. Expensive against the
// distance terms on purpose: an order arriving in pieces is a packing problem
// downstream.
//
// Counted per trolley here (distinct orders on this trolley) rather than per
// order (distinct trolleys of this order); both sum to the same number of
// (order, trolley) pairs, and the per-trolley form is the one that decomposes.
struct MinimizeOrderSplitByTrolley final : OpUnitConstraint
{
    explicit MinimizeOrderSplitByTrolley(double weight = 1000.0)
        : OpUnitConstraint("minimizeOrderSplitByTrolley",
                           weight,
                           /*hard=*/false)
    {
    }
    void onReset(const OrderPickingSolution &solution) override
    {
        seen_.assign(solution.orders.size(), 0);
    }
    double cost(const OrderPickingSolution &solution,
                int /*unit*/,
                const std::vector<int> &members) const override
    {
        touched_.clear();
        for (int pickIndex : members)
        {
            const int orderIndex = solution.pickTasks[pickIndex].orderIndex;
            if (seen_[orderIndex])
                continue;
            seen_[orderIndex] = 1;
            touched_.push_back(orderIndex);
        }
        for (int orderIndex : touched_)
            seen_[orderIndex] = 0;  // leave the scratch zeroed
        return static_cast<double>(touched_.size());
    }

private:
    mutable std::vector<char> seen_;
    mutable std::vector<int> touched_;
};

}  // namespace op

#endif  // OP_CONSTRAINTS_MINIMIZE_ORDER_SPLIT_BY_TROLLEY_HPP

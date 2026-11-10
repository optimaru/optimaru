#ifndef OP_CONSTRAINTS_ORDER_SPLIT_BY_TROLLEY_COST_HPP
#define OP_CONSTRAINTS_ORDER_SPLIT_BY_TROLLEY_COST_HPP

#include "op/constraints/common.hpp"

namespace op
{

// Soft, weight 1'000: orderSplitByTrolleyCost — one point per (order,
// trolley) pairing, so an order carried by three trolleys costs three times as
// much as one carried by a single trolley. Expensive against the distance
// terms on purpose: an order arriving in pieces is a packing problem
// downstream.
//
// Raw cost per trolley = the number of distinct orders with a pick on it.
// NOT "0 = no split" (kept as the quickstart defines it): an order carried
// whole by one trolley still costs 1, so the total is never below the number
// of orders that have assigned picks; only an empty trolley costs 0.
//
// Counted per trolley here (distinct orders on this trolley) rather than per
// order (distinct trolleys of this order); both sum to the same number of
// (order, trolley) pairs, and the per-trolley form is the one that decomposes.
struct OrderSplitByTrolleyCost final : OpUnitConstraint
{
    static constexpr double kDefaultWeight = 1000.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit OrderSplitByTrolleyCost(double weight = kDefaultWeight)
        : OpUnitConstraint("orderSplitByTrolleyCost", weight, kDefaultLevel)
    {
    }
    void on_reset(const OrderPickingSolution &solution) override
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

#endif  // OP_CONSTRAINTS_ORDER_SPLIT_BY_TROLLEY_COST_HPP

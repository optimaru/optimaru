#ifndef OP_CONSTRAINTS_TROLLEY_CAPACITY_COST_HPP
#define OP_CONSTRAINTS_TROLLEY_CAPACITY_COST_HPP

#include "op/constraints/common.hpp"

namespace op
{

// Hard, weight 1: trolleyCapacityCost — how many buckets a trolley is short
// of. Raw cost per
// trolley = max(0, required buckets - bucketCount), so 0 = the trolley carries
// enough buckets. Each ORDER on the trolley rounds its own volume up to a
// whole number of buckets, because buckets are never shared between orders;
// that rounding is exactly what makes splitting an order across trolleys
// waste capacity.
struct TrolleyCapacityCost final : OpUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TrolleyCapacityCost(double weight = kDefaultWeight)
        : OpUnitConstraint("trolleyCapacityCost", weight, kDefaultLevel)
    {
    }
    void on_reset(const OrderPickingSolution &solution) override
    {
        volume_.assign(solution.orders.size(), 0);
    }
    double cost(const OrderPickingSolution &solution,
                int unit,
                const std::vector<int> &members) const override
    {
        const Trolley &trolley = solution.trolleys[unit];
        touched_.clear();
        for (int pickIndex : members)
        {
            const PickTask &task = solution.pickTasks[pickIndex];
            if (volume_[task.orderIndex] == 0)
                touched_.push_back(task.orderIndex);
            volume_[task.orderIndex] += task.volume;
        }
        std::int64_t requiredBuckets = 0;
        for (int orderIndex : touched_)
        {
            requiredBuckets += ScoreCalculator::calculateOrderRequiredBuckets(
                volume_[orderIndex], trolley.bucketCapacity);
            volume_[orderIndex] = 0;  // leave the scratch zeroed
        }
        return requiredBuckets > trolley.bucketCount
                   ? static_cast<double>(requiredBuckets - trolley.bucketCount)
                   : 0.0;
    }

private:
    // Per-order volume scratch, sized once per reset and always left zeroed
    // (cost() is const by contract; the calculator drives one solve).
    mutable std::vector<std::int64_t> volume_;
    mutable std::vector<int> touched_;
};

}  // namespace op

#endif  // OP_CONSTRAINTS_TROLLEY_CAPACITY_COST_HPP

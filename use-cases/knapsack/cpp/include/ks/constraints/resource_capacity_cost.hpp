#ifndef KS_CONSTRAINTS_RESOURCE_CAPACITY_COST_HPP
#define KS_CONSTRAINTS_RESOURCE_CAPACITY_COST_HPP

#include <cstdint>
#include <vector>

#include "ks/constraints/common.hpp"

namespace ks
{

// Hard, weight 1: resourceCapacityCost — the selected usage beyond each
// declared resource's capacity (the multidimensional knapsack). Raw cost =
// sum over declared resources r of max(0, selected usage of r - capacity of
// r), one total; usage of an undeclared resource is ignored; 0 = every
// resource fits. Keeps the usage per resource and the overflow sum, O(the
// item's resources) per flip.
//
// One total (a global constraint): the exponent shapes the summed overflow,
// not each resource's (0.1.x shaped each resource; see the package README).
struct ResourceCapacityCost final : detail::SelectionConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ResourceCapacityCost(double weight = kDefaultWeight)
        : detail::SelectionConstraint("resourceCapacityCost", weight, kDefaultLevel)
    {
    }

    void reset(const KnapsackProblem &problem,
               const std::vector<std::vector<int>> &members_per_unit) override
    {
        detail::requirePrepared(problem);
        capacity_.clear();
        for (const Resource &resource : problem.resources)
            capacity_.push_back(resource.capacity);
        usage_.assign(capacity_.size(), 0);
        const std::vector<char> selected = detail::selectedFlags(problem, members_per_unit);
        for (std::size_t i = 0; i < selected.size(); ++i)
        {
            if (!selected[i])
                continue;
            for (const auto &[resource, amount] : problem.itemResources[i])
                usage_[resource] += amount;
        }
        overflow_ = 0;
        for (std::size_t r = 0; r < usage_.size(); ++r)
            overflow_ += overflowOf(static_cast<int>(r));
    }

    void onSelectionChanged(const KnapsackProblem &problem, int item, bool selected) override
    {
        for (const auto &[resource, amount] : problem.itemResources[item])
        {
            overflow_ -= overflowOf(resource);
            usage_[resource] += selected ? amount : -amount;
            overflow_ += overflowOf(resource);
        }
    }

    double cost() const override { return static_cast<double>(overflow_); }

private:
    std::int64_t overflowOf(int resource) const
    {
        const std::int64_t over = usage_[resource] - capacity_[resource];
        return over > 0 ? over : 0;
    }

    std::vector<std::int64_t> capacity_;
    std::vector<std::int64_t> usage_;
    std::int64_t overflow_ = 0;
};

}  // namespace ks

#endif  // KS_CONSTRAINTS_RESOURCE_CAPACITY_COST_HPP

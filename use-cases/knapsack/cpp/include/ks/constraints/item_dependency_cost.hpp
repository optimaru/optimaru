#ifndef KS_CONSTRAINTS_ITEM_DEPENDENCY_COST_HPP
#define KS_CONSTRAINTS_ITEM_DEPENDENCY_COST_HPP

#include <cstdint>
#include <vector>

#include "ks/constraints/common.hpp"

namespace ks
{

// Hard, weight 1: itemDependencyCost — selected items whose required item is
// left out. Raw cost = the number of (item, required) pairs — every entry of
// every item's requiredItems, duplicates separately — with the item selected
// and the required item not (undecided = not selected), one total; a
// self-dependency never counts; 0 = every dependency is met. Keeps its own
// copy of the selection and re-evaluates the pairs of the flipped item,
// O(the item's dependencies) per flip.
//
// One total (a global constraint): the exponent shapes the count (0.1.x
// shaped each 0/1 pair; see the README).
struct ItemDependencyCost final : detail::SelectionConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ItemDependencyCost(double weight = kDefaultWeight)
        : detail::SelectionConstraint("itemDependencyCost", weight, kDefaultLevel)
    {
    }

    void reset(const KnapsackProblem &problem,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        detail::requirePrepared(problem);
        selected_ = detail::selectedFlags(problem, membersPerUnit);
        unmet_ = 0;
        for (const Dependency &dependency : problem.dependencies)
            unmet_ += unmetOf(dependency);
    }

    void onSelectionChanged(const KnapsackProblem &problem, int item, bool selected) override
    {
        for (int d : problem.dependenciesAsItem[item])
            unmet_ -= unmetOf(problem.dependencies[d]);
        for (int d : problem.dependenciesAsRequired[item])
            unmet_ -= unmetOf(problem.dependencies[d]);
        selected_[item] = selected ? 1 : 0;
        for (int d : problem.dependenciesAsItem[item])
            unmet_ += unmetOf(problem.dependencies[d]);
        for (int d : problem.dependenciesAsRequired[item])
            unmet_ += unmetOf(problem.dependencies[d]);
    }

    double cost() const override { return static_cast<double>(unmet_); }

private:
    std::int64_t unmetOf(const Dependency &dependency) const
    {
        return selected_[dependency.item] && !selected_[dependency.required] ? 1 : 0;
    }

    std::vector<char> selected_;
    std::int64_t unmet_ = 0;
};

}  // namespace ks

#endif  // KS_CONSTRAINTS_ITEM_DEPENDENCY_COST_HPP

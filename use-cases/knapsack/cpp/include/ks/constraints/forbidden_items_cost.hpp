#ifndef KS_CONSTRAINTS_FORBIDDEN_ITEMS_COST_HPP
#define KS_CONSTRAINTS_FORBIDDEN_ITEMS_COST_HPP

#include <cstdint>
#include <vector>

#include "ks/constraints/common.hpp"

namespace ks
{

// Hard, weight 1: forbiddenItemsCost — forbidden items selected. Raw cost =
// the number of forbiddenItems entries whose item is selected (an item listed
// twice counts twice), one total; 0 = no forbidden item is selected. O(1) per
// flip.
//
// One total (a global constraint): the exponent shapes the count.
struct ForbiddenItemsCost final : detail::SelectionConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ForbiddenItemsCost(double weight = kDefaultWeight)
        : detail::SelectionConstraint("forbiddenItemsCost", weight, kDefaultLevel)
    {
    }

    void reset(const KnapsackProblem &problem,
               const std::vector<std::vector<int>> &members_per_unit) override
    {
        detail::requirePrepared(problem);
        chosen_ = 0;
        const std::vector<char> selected = detail::selectedFlags(problem, members_per_unit);
        for (std::size_t i = 0; i < selected.size(); ++i)
        {
            if (selected[i])
                chosen_ += problem.forbiddenCount[i];
        }
    }

    void onSelectionChanged(const KnapsackProblem &problem, int item, bool selected) override
    {
        const std::int64_t entries = problem.forbiddenCount[item];
        chosen_ += selected ? entries : -entries;
    }

    double cost() const override { return static_cast<double>(chosen_); }

private:
    std::int64_t chosen_ = 0;
};

}  // namespace ks

#endif  // KS_CONSTRAINTS_FORBIDDEN_ITEMS_COST_HPP

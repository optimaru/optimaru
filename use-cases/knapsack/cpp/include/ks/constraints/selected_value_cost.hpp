#ifndef KS_CONSTRAINTS_SELECTED_VALUE_COST_HPP
#define KS_CONSTRAINTS_SELECTED_VALUE_COST_HPP

#include <cstdint>
#include <vector>

#include "ks/constraints/common.hpp"

namespace ks
{

// Soft, weight 1, REWARD: selectedValueCost — the value of the selection, the
// knapsack objective. Raw cost = -(sum of selected values), one total, always
// <= 0, so the soft score is +selected value; 0 = nothing of value is
// selected (a reward has no "violation"). As a reward it can never be hard
// (level='hard' is refused) and its level cannot be changed. With an exponent e
// the contribution is -(value^e). Keeps the selected value, O(1) per flip.
struct SelectedValueCost final : detail::SelectionConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit SelectedValueCost(double weight = kDefaultWeight)
        : detail::SelectionConstraint("selectedValueCost", weight, kDefaultLevel)
    {
        reward = true;  // raw cost <= 0; level='hard' refused everywhere
        levelFlippable = false;
    }

    void reset(const KnapsackProblem &problem,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        detail::requirePrepared(problem);
        value_ = 0;
        const std::vector<char> selected = detail::selectedFlags(problem, membersPerUnit);
        for (std::size_t i = 0; i < selected.size(); ++i)
        {
            if (selected[i])
                value_ += problem.items[i].value;
        }
    }

    void onSelectionChanged(const KnapsackProblem &problem, int item, bool selected) override
    {
        const std::int64_t value = problem.items[item].value;
        value_ += selected ? value : -value;
    }

    double cost() const override { return value_ == 0 ? 0.0 : -static_cast<double>(value_); }

private:
    std::int64_t value_ = 0;
};

}  // namespace ks

#endif  // KS_CONSTRAINTS_SELECTED_VALUE_COST_HPP

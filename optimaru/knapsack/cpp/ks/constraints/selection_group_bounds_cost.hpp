#ifndef KS_CONSTRAINTS_SELECTION_GROUP_BOUNDS_COST_HPP
#define KS_CONSTRAINTS_SELECTION_GROUP_BOUNDS_COST_HPP

#include <cstdint>
#include <vector>

#include "ks/constraints/common.hpp"

namespace ks {

// Hard, weight 1: selectionGroupBoundsCost — selection groups outside their
// bounds. With n = the selected items of group g, raw cost = sum over groups
// of max(0, minSelected - n) + max(0, n - maxSelected) (no maxSelected = the
// group size, so no excess), one total; 0 = every group is within its
// bounds. Keeps the count per group and the violation sum, O(the item's
// groups) per flip.
//
// One total (a global constraint): the exponent shapes the summed violation,
// not each group's.
struct SelectionGroupBoundsCost final : detail::SelectionConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit SelectionGroupBoundsCost(double weight = kDefaultWeight)
        : detail::SelectionConstraint("selectionGroupBoundsCost", weight, kDefaultLevel) {}

    void reset(const KnapsackProblem& problem,
               const std::vector<std::vector<int>>& members_per_unit) override {
        detail::requirePrepared(problem);
        minimum_.clear();
        maximum_.clear();
        for (const SelectionGroup& group : problem.selectionGroups) {
            minimum_.push_back(group.minSelected);
            maximum_.push_back(group.effectiveMax());
        }
        count_.assign(minimum_.size(), 0);
        const std::vector<char> selected = detail::selectedFlags(problem, members_per_unit);
        for (std::size_t i = 0; i < selected.size(); ++i) {
            if (!selected[i]) continue;
            for (int group : problem.itemGroups[i]) ++count_[group];
        }
        violation_ = 0;
        for (std::size_t g = 0; g < count_.size(); ++g)
            violation_ += violationOf(static_cast<int>(g));
    }

    void onSelectionChanged(const KnapsackProblem& problem, int item, bool selected) override {
        for (int group : problem.itemGroups[item]) {
            violation_ -= violationOf(group);
            count_[group] += selected ? 1 : -1;
            violation_ += violationOf(group);
        }
    }

    double cost() const override { return static_cast<double>(violation_); }

private:
    std::int64_t violationOf(int group) const {
        const std::int64_t n = count_[group];
        const std::int64_t under = minimum_[group] - n;
        const std::int64_t over = n - maximum_[group];
        return (under > 0 ? under : 0) + (over > 0 ? over : 0);
    }

    std::vector<std::int64_t> minimum_;
    std::vector<std::int64_t> maximum_;
    std::vector<std::int64_t> count_;
    std::int64_t violation_ = 0;
};

}  // namespace ks

#endif  // KS_CONSTRAINTS_SELECTION_GROUP_BOUNDS_COST_HPP

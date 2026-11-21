#ifndef KS_CONSTRAINTS_MANDATORY_ITEMS_COST_HPP
#define KS_CONSTRAINTS_MANDATORY_ITEMS_COST_HPP

#include <cstdint>
#include <vector>

#include "ks/constraints/common.hpp"

namespace ks {

// Hard, weight 1: mandatoryItemsCost — mandatory items left out. Raw cost =
// the number of mandatoryItems entries whose item is not selected (an item
// listed twice counts twice; undecided counts as not selected), one total;
// 0 = every mandatory item is selected. O(1) per flip.
//
// One total (a global constraint): the exponent shapes the count.
struct MandatoryItemsCost final : detail::SelectionConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MandatoryItemsCost(double weight = kDefaultWeight)
        : detail::SelectionConstraint("mandatoryItemsCost", weight, kDefaultLevel) {}

    void reset(const KnapsackProblem& problem,
               const std::vector<std::vector<int>>& members_per_unit) override {
        detail::requirePrepared(problem);
        missing_ = 0;
        const std::vector<char> selected = detail::selectedFlags(problem, members_per_unit);
        for (std::size_t i = 0; i < selected.size(); ++i) {
            if (!selected[i]) missing_ += problem.mandatoryCount[i];
        }
    }

    void onSelectionChanged(const KnapsackProblem& problem, int item, bool selected) override {
        const std::int64_t entries = problem.mandatoryCount[item];
        missing_ += selected ? -entries : entries;
    }

    double cost() const override { return static_cast<double>(missing_); }

private:
    std::int64_t missing_ = 0;
};

}  // namespace ks

#endif  // KS_CONSTRAINTS_MANDATORY_ITEMS_COST_HPP

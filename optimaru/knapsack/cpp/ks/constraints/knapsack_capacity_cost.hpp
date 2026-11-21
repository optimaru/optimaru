#ifndef KS_CONSTRAINTS_KNAPSACK_CAPACITY_COST_HPP
#define KS_CONSTRAINTS_KNAPSACK_CAPACITY_COST_HPP

#include <cstdint>
#include <vector>

#include "ks/constraints/common.hpp"

namespace ks {

// Hard, weight 1: knapsackCapacityCost — the selected weight beyond the
// knapsack's capacity. Raw cost = max(0, sum of selected weights - capacity),
// one total; 0 = the selection fits. Keeps the selected load, O(1) per flip.
struct KnapsackCapacityCost final : detail::SelectionConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit KnapsackCapacityCost(double weight = kDefaultWeight)
        : detail::SelectionConstraint("knapsackCapacityCost", weight, kDefaultLevel) {}

    void reset(const KnapsackProblem& problem,
               const std::vector<std::vector<int>>& members_per_unit) override {
        detail::requirePrepared(problem);
        capacity_ = problem.capacity;
        load_ = 0;
        const std::vector<char> selected = detail::selectedFlags(problem, members_per_unit);
        for (std::size_t i = 0; i < selected.size(); ++i) {
            if (selected[i]) load_ += problem.items[i].weight;
        }
    }

    void onSelectionChanged(const KnapsackProblem& problem, int item, bool selected) override {
        const std::int64_t weight = problem.items[item].weight;
        load_ += selected ? weight : -weight;
    }

    double cost() const override {
        const std::int64_t over = load_ - capacity_;
        return over > 0 ? static_cast<double>(over) : 0.0;
    }

private:
    std::int64_t capacity_ = 0;
    std::int64_t load_ = 0;
};

}  // namespace ks

#endif  // KS_CONSTRAINTS_KNAPSACK_CAPACITY_COST_HPP

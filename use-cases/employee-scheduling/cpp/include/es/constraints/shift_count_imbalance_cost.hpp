#ifndef ES_CONSTRAINTS_SHIFT_COUNT_IMBALANCE_COST_HPP
#define ES_CONSTRAINTS_SHIFT_COUNT_IMBALANCE_COST_HPP

#include "es/constraints/common.hpp"

namespace es {

// Soft, weight 1, global: shiftCountImbalanceCost, the load-balancing
// "unfairness" over the per-employee shift counts: sqrt(sum of squared
// deviations from the mean count), rounded to 6 significant figures (every
// employee counts, including those with no shift). Not a per-unit sum, so it
// maintains the count aggregates itself and yields the closed-form total at
// scoring time. 0 = every employee holds the same number of shifts.
struct ShiftCountImbalanceCost final : EsGlobalConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit ShiftCountImbalanceCost(double weight = kDefaultWeight)
        : EsGlobalConstraint("shiftCountImbalanceCost", weight, kDefaultLevel) {}

    void reset(const EmployeeSchedule& /*s*/,
               const std::vector<std::vector<int>>& members_per_unit) override {
        n_ = static_cast<int>(members_per_unit.size());
        counts_.assign(n_, 0);
        sum_ = 0;
        sumSq_ = 0;
        for (int u = 0; u < n_; ++u) {
            long c = static_cast<long>(members_per_unit[u].size());
            counts_[u] = c;
            sum_ += c;
            sumSq_ += c * c;
        }
    }
    void on_unit_changed(const EmployeeSchedule& /*s*/, int unit,
                       const std::vector<int>& members) override {
        long oldCount = counts_[unit];
        long newCount = static_cast<long>(members.size());
        sum_ += newCount - oldCount;
        sumSq_ += newCount * newCount - oldCount * oldCount;
        counts_[unit] = newCount;
    }
    bool supports_member_change() const override { return true; }
    void on_member_changed(const EmployeeSchedule& s, int unit,
                         const std::vector<int>& members, int, bool) override {
        on_unit_changed(s, unit, members);
    }
    double cost() const override {
        return ScoreCalculator::unfairnessFromAggregates(sum_, sumSq_, n_);
    }

private:
    std::vector<long> counts_;
    long sum_ = 0;
    long sumSq_ = 0;
    int n_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_SHIFT_COUNT_IMBALANCE_COST_HPP

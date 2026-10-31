#ifndef ES_CONSTRAINTS_BALANCE_MIN_MAX_GAP_HPP
#define ES_CONSTRAINTS_BALANCE_MIN_MAX_GAP_HPP

#include "es/constraints/common.hpp"

namespace es {

// Soft (default): how far the busiest employee is above the least busy one,
// beyond `tolerance` (fairness as the min-max gap of the per-employee shift
// counts). Amortized constant-time maintenance via a count histogram.
struct BalanceMinMaxGap final : EsGlobalConstraint {
    explicit BalanceMinMaxGap(double weight = 1.0, int tolerance = 0)
        : EsGlobalConstraint("Balance min-max gap", weight, /*hard=*/false),
          tolerance_(tolerance) {}
    void reset(const EmployeeSchedule& s,
               const std::vector<std::vector<int>>& membersPerUnit) override {
        counts_.assign(membersPerUnit.size(), 0);
        hist_.assign(s.shifts.size() + 2, 0);
        for (std::size_t u = 0; u < membersPerUnit.size(); ++u) {
            counts_[u] = static_cast<int>(membersPerUnit[u].size());
            ++hist_[counts_[u]];
        }
        minC_ = 0;
        maxC_ = static_cast<int>(hist_.size()) - 1;
        tighten();
    }
    void onUnitChanged(const EmployeeSchedule&, int unit,
                       const std::vector<int>& members) override {
        const int oldC = counts_[unit];
        const int newC = static_cast<int>(members.size());
        if (newC == oldC) return;
        --hist_[oldC];
        ++hist_[newC];
        counts_[unit] = newC;
        minC_ = std::min(minC_, newC);
        maxC_ = std::max(maxC_, newC);
        tighten();
    }
    double cost() const override {
        if (counts_.size() < 2) return 0.0;
        const int gap = maxC_ - minC_ - tolerance_;
        return gap > 0 ? static_cast<double>(gap) : 0.0;
    }

private:
    void tighten() {
        while (minC_ < maxC_ && hist_[minC_] == 0) ++minC_;
        while (maxC_ > minC_ && hist_[maxC_] == 0) --maxC_;
    }
    int tolerance_;
    std::vector<int> counts_;   // per employee: assigned shift count
    std::vector<int> hist_;     // count value -> number of employees
    int minC_ = 0, maxC_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_BALANCE_MIN_MAX_GAP_HPP

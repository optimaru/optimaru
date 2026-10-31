#ifndef ES_CONSTRAINTS_BALANCE_LOCATION_HPP
#define ES_CONSTRAINTS_BALANCE_LOCATION_HPP

#include "es/constraints/common.hpp"

namespace es {

// Soft, global fairness of a per-employee count across employees that have
// `workerSkill` (empty = all). The counted quantity is either shifts of a
// `location` (empty = all work), or — when `offTargetDays` is non-empty — the
// number of those target days each employee has OFF. `metric` selects "ssd"
// (sqrt of the sum of squared deviations, matching BalanceShiftAssignments) or
// "gap" (max-min beyond `tolerance`). Covers balance per location, a gap on one
// location's count, and balance of days off over a set of target days.
struct BalanceLocation final : EsGlobalConstraint {
    BalanceLocation(double weight, std::string location, std::string workerSkill,
                    std::string metric, int tolerance, std::vector<int> offTargetDays)
        : EsGlobalConstraint("Balance[" +
                                     (!offTargetDays.empty()
                                              ? std::string("off-target")
                                              : (location.empty() ? std::string("work") : location)) +
                                     " " + (workerSkill.empty() ? "all" : workerSkill) + " " + metric +
                                     "]",
                             weight, /*hard=*/false),
          loc_(std::move(location)), skill_(std::move(workerSkill)), metric_(std::move(metric)),
          tolerance_(tolerance), target_(std::move(offTargetDays)) {}
    void reset(const EmployeeSchedule& s,
               const std::vector<std::vector<int>>& membersPerUnit) override {
        match_.resize(s.shifts.size());
        day_.resize(s.shifts.size());
        horizon_ = 0;
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            match_[i] = (loc_.empty() || s.shifts[i].location == loc_) ? 1 : 0;
            day_[i] = static_cast<std::int32_t>(s.shifts[i].startDay);
            horizon_ = std::max(horizon_, day_[i] + 1);
        }
        useOff_ = !target_.empty();
        isTarget_.assign(horizon_, 0);
        nTarget_ = 0;
        if (useOff_)
            for (int d : target_)
                if (d >= 0 && d < horizon_ && !isTarget_[d]) { isTarget_[d] = 1; ++nTarget_; }
        applies_.assign(membersPerUnit.size(), skill_.empty() ? 1 : 0);
        if (!skill_.empty())
            for (std::size_t e = 0; e < s.employees.size(); ++e)
                applies_[e] = s.employees[e].skills.count(skill_) ? 1 : 0;
        seen_.assign(horizon_, 0);
        counts_.assign(membersPerUnit.size(), 0);
        sum_ = 0;
        sumSq_ = 0;
        applicableCount_ = 0;
        histogram_.assign(std::max(s.shifts.size(), static_cast<std::size_t>(horizon_)) + 2, 0);
        minCount_ = static_cast<int>(histogram_.size()) - 1;
        maxCount_ = 0;
        gapMetric_ = metric_ == "gap";
        for (std::size_t u = 0; u < membersPerUnit.size(); ++u) {
            counts_[u] = computeCount(membersPerUnit[u]);
            if (!applies_[u]) continue;
            const long count = counts_[u];
            sum_ += count;
            sumSq_ += count * count;
            ++applicableCount_;
            ++histogram_[count];
            minCount_ = std::min(minCount_, static_cast<int>(count));
            maxCount_ = std::max(maxCount_, static_cast<int>(count));
        }
        tightenHistogram();
    }
    void onUnitChanged(const EmployeeSchedule&, int unit,
                       const std::vector<int>& members) override {
        if (!applies_[unit]) return;
        updateCount(unit, computeCount(members));
    }
    double cost() const override {
        if (applicableCount_ < 2) return 0.0;
        if (gapMetric_) {
            const long gap = maxCount_ - minCount_ - tolerance_;
            return gap > 0 ? static_cast<double>(gap) : 0.0;
        }
        return ScoreCalculator::unfairnessFromAggregates(sum_, sumSq_, applicableCount_);
    }

private:
    void updateCount(int unit, long newCount) {
        const long oldCount = counts_[unit];
        if (newCount == oldCount) return;
        counts_[unit] = newCount;
        sum_ += newCount - oldCount;
        sumSq_ += newCount * newCount - oldCount * oldCount;
        --histogram_[oldCount];
        ++histogram_[newCount];
        minCount_ = std::min(minCount_, static_cast<int>(newCount));
        maxCount_ = std::max(maxCount_, static_cast<int>(newCount));
        tightenHistogram();
    }
    void tightenHistogram() {
        if (applicableCount_ == 0) {
            minCount_ = maxCount_ = 0;
            return;
        }
        while (minCount_ < maxCount_ && histogram_[minCount_] == 0) ++minCount_;
        while (maxCount_ > minCount_ && histogram_[maxCount_] == 0) --maxCount_;
    }
    long computeCount(const std::vector<int>& members) const {
        if (useOff_) {
            long worked = 0;
            for (int i : members)
                if (isTarget_[day_[i]] && !seen_[day_[i]]) { seen_[day_[i]] = 1; ++worked; }
            for (int i : members) seen_[day_[i]] = 0;
            return nTarget_ - worked;  // off days among the target set
        }
        long c = 0;
        for (int i : members) c += match_[i];
        return c;
    }
    std::string loc_, skill_, metric_;
    int tolerance_;
    std::vector<int> target_;
    bool useOff_ = false;
    std::vector<char> match_, applies_, isTarget_;
    std::vector<std::int32_t> day_;
    mutable std::vector<char> seen_;
    std::vector<long> counts_;
    int nTarget_ = 0;
    std::int32_t horizon_ = 0;
    long sum_ = 0;
    long sumSq_ = 0;
    int applicableCount_ = 0;
    std::vector<int> histogram_;
    int minCount_ = 0;
    int maxCount_ = 0;
    bool gapMetric_ = false;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_BALANCE_LOCATION_HPP

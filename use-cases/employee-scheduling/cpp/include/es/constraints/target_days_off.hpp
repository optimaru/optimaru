#ifndef ES_CONSTRAINTS_TARGET_DAYS_OFF_HPP
#define ES_CONSTRAINTS_TARGET_DAYS_OFF_HPP

#include "es/constraints/common.hpp"

namespace es {

// Soft: per employee, at least `minCount` of the given `targetDays` (day
// indices, e.g. weekends + holidays) must be OFF (no work). Penalty = shortfall.
struct TargetDaysOff final : EsUnitConstraint {
    TargetDaysOff(double weight, std::vector<int> targetDays, int minCount)
        : EsUnitConstraint("Target days off[>=" + std::to_string(minCount) + "]", weight,
                           /*hard=*/false),
          target_(std::move(targetDays)), min_(minCount) {}
    void onReset(const EmployeeSchedule& s) override {
        horizon_ = 0;
        day_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            day_[i] = static_cast<std::int32_t>(s.shifts[i].startDay);
            horizon_ = std::max(horizon_, day_[i] + 1);
        }
        isTarget_.assign(horizon_, 0);
        nTarget_ = 0;
        for (int d : target_)
            if (d >= 0 && d < horizon_ && !isTarget_[d]) { isTarget_[d] = 1; ++nTarget_; }
        work_.assign(horizon_, 0);
    }
    double cost(const EmployeeSchedule&, int, const std::vector<int>& members) const override {
        int workedTargets = 0;
        for (int i : members)
            if (isTarget_[day_[i]] && !work_[day_[i]]) { work_[day_[i]] = 1; ++workedTargets; }
        const int off = nTarget_ - workedTargets;
        double c = off < min_ ? static_cast<double>(min_ - off) : 0.0;
        for (int i : members) work_[day_[i]] = 0;
        return c;
    }

private:
    std::vector<int> target_;
    int min_;
    std::vector<std::int32_t> day_;
    std::vector<char> isTarget_;
    mutable std::vector<char> work_;
    int nTarget_ = 0;
    std::int32_t horizon_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_TARGET_DAYS_OFF_HPP

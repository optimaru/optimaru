#ifndef ES_CONSTRAINTS_REST_AFTER_CONSECUTIVE_WORK_COST_HPP
#define ES_CONSTRAINTS_REST_AFTER_CONSECUTIVE_WORK_COST_HPP

#include "es/constraints/common.hpp"

namespace es {

// Hard, weight 1: restAfterConsecutiveWorkCost. After a run of >= `workDays`
// consecutive worked days the employee must get >= `restDays` consecutive
// rest days before working again. Raw cost per employee = restDays - gap for
// each qualifying run whose following rest gap is too short, so 0 = every
// long run is followed by enough rest. A run at the end of the horizon (no
// subsequent work) is not penalized — there is no "next work" to rest before.
// Defaults: 6 worked days require 3 rest days, so with
// MaxConsecutiveDaysCost(6) this yields a six days on, three off pattern.
// restDays must be above 1 to bind: any rest gap is at least 1 day. A day
// counts as worked if it holds any shift.
struct RestAfterConsecutiveWorkCost final : EsUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RestAfterConsecutiveWorkCost(double weight = kDefaultWeight, int workDays = 6,
                                          int restDays = 3)
        : EsUnitConstraint("restAfterConsecutiveWorkCost", weight, kDefaultLevel),
          workDays_(workDays), restDays_(restDays) {}
    void onReset(const EmployeeSchedule& s) override {
        horizon_ = 0;
        day_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            day_[i] = static_cast<std::int32_t>(s.shifts[i].startDay);
            horizon_ = std::max(horizon_, day_[i] + 1);
        }
        worked_.assign(horizon_, 0);
    }
    double cost(const EmployeeSchedule&, int /*unit*/,
                const std::vector<int>& members) const override {
        for (int i : members) worked_[day_[i]] = 1;
        double c = 0;
        std::int32_t run = 0;         // current run of consecutive worked days
        std::int32_t gap = 0;         // current run of consecutive rest days
        std::int32_t priorRun = 0;    // worked-run preceding the current gap
        for (std::int32_t d = 0; d < horizon_; ++d) {
            if (worked_[d]) {
                if (gap > 0) {  // a rest gap ends because work resumes here
                    if (priorRun >= workDays_ && gap < restDays_)
                        c += restDays_ - gap;
                    gap = 0;
                    priorRun = 0;
                }
                ++run;
            } else {
                if (run > 0) { priorRun = run; run = 0; }
                ++gap;
            }
        }
        for (int i : members) worked_[day_[i]] = 0;  // restore the scratch
        return c;
    }

private:
    std::int32_t workDays_, restDays_;
    std::vector<std::int32_t> day_;      // shift -> start day
    mutable std::vector<char> worked_;   // scratch, zeroed after use
    std::int32_t horizon_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_REST_AFTER_CONSECUTIVE_WORK_COST_HPP

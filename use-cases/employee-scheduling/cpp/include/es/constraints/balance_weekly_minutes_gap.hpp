#ifndef ES_CONSTRAINTS_BALANCE_WEEKLY_MINUTES_GAP_HPP
#define ES_CONSTRAINTS_BALANCE_WEEKLY_MINUTES_GAP_HPP

#include "es/constraints/common.hpp"

namespace es {

// Soft (default): per calendar week (7-day blocks from day 0), the gap between
// the busiest and least-busy employee's assigned minutes, beyond
// `toleranceMinutes`, summed over weeks (fairness of weekly hours). The raw
// cost is expressed in HOURS (excess minutes / 60) so it sits on a comparable
// scale to the other soft terms; the threshold stays precise in minutes. ALL
// employees are compared, so one who works zero minutes in a week counts as 0
// (the min may be 0). Global because it is a min-max statistic across
// employees, not a per-employee sum. cost() recomputes each week's min/max
// from the maintained per-(employee, week) minute totals (cost proportional to
// employees x weeks); onUnitChanged re-tallies only the changed employee's row.
struct BalanceWeeklyMinutesGap final : EsGlobalConstraint {
    explicit BalanceWeeklyMinutesGap(double weight = 1.0, int toleranceMinutes = 120)
        : EsGlobalConstraint("Balance weekly minutes gap", weight, /*hard=*/false),
          tolerance_(toleranceMinutes) {}
    void reset(const EmployeeSchedule& s,
               const std::vector<std::vector<int>>& membersPerUnit) override {
        units_ = static_cast<int>(membersPerUnit.size());
        weeks_ = 0;
        shiftWeek_.resize(s.shifts.size());
        shiftDur_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            shiftWeek_[i] = static_cast<int>(s.shifts[i].startDay / 7);
            shiftDur_[i] = static_cast<std::int64_t>(s.shifts[i].end - s.shifts[i].start);
            weeks_ = std::max(weeks_, shiftWeek_[i] + 1);
        }
        weekMinutes_.assign(static_cast<std::size_t>(units_) * weeks_, 0);
        for (int u = 0; u < units_; ++u) recomputeUnit(u, membersPerUnit[u]);
    }
    void onUnitChanged(const EmployeeSchedule&, int unit,
                       const std::vector<int>& members) override {
        recomputeUnit(unit, members);
    }
    double cost() const override {
        if (units_ < 2 || weeks_ == 0) return 0.0;
        double c = 0;
        for (int w = 0; w < weeks_; ++w) {
            std::int64_t mn = weekMinutes_[w], mx = weekMinutes_[w];  // unit 0
            for (int u = 1; u < units_; ++u) {
                const std::int64_t v =
                        weekMinutes_[static_cast<std::size_t>(u) * weeks_ + w];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            const std::int64_t gap = mx - mn - tolerance_;
            if (gap > 0) c += static_cast<double>(gap) / 60.0;  // minutes -> hours
        }
        return c;
    }

private:
    void recomputeUnit(int unit, const std::vector<int>& members) {
        std::int64_t* row =
                weekMinutes_.data() + static_cast<std::size_t>(unit) * weeks_;
        for (int w = 0; w < weeks_; ++w) row[w] = 0;
        for (int i : members) row[shiftWeek_[i]] += shiftDur_[i];
    }
    int tolerance_;
    int units_ = 0, weeks_ = 0;
    std::vector<int> shiftWeek_;             // shift -> week
    std::vector<std::int64_t> shiftDur_;     // shift -> duration minutes
    std::vector<std::int64_t> weekMinutes_;  // unit*weeks + week -> minutes
};

}  // namespace es

#endif  // ES_CONSTRAINTS_BALANCE_WEEKLY_MINUTES_GAP_HPP

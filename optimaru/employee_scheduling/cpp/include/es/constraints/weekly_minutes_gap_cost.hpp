#ifndef ES_CONSTRAINTS_WEEKLY_MINUTES_GAP_COST_HPP
#define ES_CONSTRAINTS_WEEKLY_MINUTES_GAP_COST_HPP

#include "es/constraints/common.hpp"

namespace es {

// Soft, weight 1, global: weeklyMinutesGapCost. Per calendar week (7-day
// blocks from day 0), the gap between the busiest and least-busy employee's
// assigned minutes, beyond `toleranceMinutes` (default 120), summed over
// weeks (fairness of weekly hours); 0 = every week is within the tolerance.
// The raw cost is expressed in HOURS (excess minutes / 60) so it sits on a
// comparable scale to the other soft terms; the threshold stays precise in
// minutes. ALL
// employees are compared, so one who works zero minutes in a week counts as 0
// (the min may be 0). Global because it is a min-max statistic across
// employees, not a per-employee sum. cost() recomputes each week's min/max
// from the maintained per-(employee, week) minute totals (cost proportional to
// employees x weeks); on_unit_changed re-tallies only the changed employee's row.
struct WeeklyMinutesGapCost final : EsGlobalConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit WeeklyMinutesGapCost(double weight = kDefaultWeight, int toleranceMinutes = 120)
        : EsGlobalConstraint("weeklyMinutesGapCost", weight, kDefaultLevel),
          tolerance_(toleranceMinutes) {}
    void reset(const EmployeeSchedule& s,
               const std::vector<std::vector<int>>& members_per_unit) override {
        units_ = static_cast<int>(members_per_unit.size());
        weeks_ = 0;
        shiftWeek_.resize(s.shifts.size());
        shiftDur_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            shiftWeek_[i] = static_cast<int>(s.shifts[i].startDay / 7);
            shiftDur_[i] = static_cast<std::int64_t>(s.shifts[i].end - s.shifts[i].start);
            weeks_ = std::max(weeks_, shiftWeek_[i] + 1);
        }
        weekMinutes_.assign(static_cast<std::size_t>(units_) * weeks_, 0);
        for (int u = 0; u < units_; ++u) recomputeUnit(u, members_per_unit[u]);
    }
    void on_unit_changed(const EmployeeSchedule&, int unit,
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

#endif  // ES_CONSTRAINTS_WEEKLY_MINUTES_GAP_COST_HPP

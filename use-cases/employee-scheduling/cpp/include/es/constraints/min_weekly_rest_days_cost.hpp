#ifndef ES_CONSTRAINTS_MIN_WEEKLY_REST_DAYS_COST_HPP
#define ES_CONSTRAINTS_MIN_WEEKLY_REST_DAYS_COST_HPP

#include "es/constraints/common.hpp"

namespace es {

// Hard, weight 1: minWeeklyRestDaysCost. Raw cost per employee = the worked
// days beyond (7 - value) in each calendar week (7-day blocks from day 0,
// matching WeeklyMinutesConstraint) — i.e. "at least `value` (default 1)
// rest days per week" — so 0 = every week leaves that much rest. A day
// counts once even with several shifts. value=1 -> at most 6 worked days per
// 7-day week. A trailing partial week can never exceed the cap, so it is
// never forced to yield rest days it has no room for; a week worked only on
// pinned days is skipped.
struct MinWeeklyRestDaysCost final : EsUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MinWeeklyRestDaysCost(double weight = kDefaultWeight, int value = 1)
        : EsUnitConstraint("minWeeklyRestDaysCost", weight, kDefaultLevel),
          maxWorkDays_(7 - value) {}
    void on_reset(const EmployeeSchedule& s) override {
        weeks_ = 0;
        horizon_ = 0;
        day_.resize(s.shifts.size());
        week_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            day_[i] = static_cast<std::int32_t>(s.shifts[i].startDay);
            week_[i] = day_[i] / 7;
            weeks_ = std::max(weeks_, week_[i] + 1);
            horizon_ = std::max(horizon_, day_[i] + 1);
        }
        worked_.assign(horizon_, 0);
        weekDays_.assign(weeks_, 0);
        movable_.assign(weeks_, 0);
    }
    double cost(const EmployeeSchedule& s, int /*unit*/,
                const std::vector<int>& members) const override {
        // Tally each distinct worked day into its week (a day counts once).
        for (int i : members) {
            if (!worked_[day_[i]]) {
                worked_[day_[i]] = 1;
                ++weekDays_[week_[i]];
            }
            if (!s.shifts[i].pinned) movable_[week_[i]] = 1;
        }
        double c = 0;
        for (std::int32_t w = 0; w < weeks_; ++w) {
            // B: a week worked entirely on pinned (fixed) days is not penalized.
            if (movable_[w] && weekDays_[w] > maxWorkDays_) c += weekDays_[w] - maxWorkDays_;
            weekDays_[w] = 0;  // restore the scratch
            movable_[w] = 0;
        }
        for (int i : members) worked_[day_[i]] = 0;  // restore the scratch
        return c;
    }

private:
    std::int32_t maxWorkDays_;
    std::vector<std::int32_t> day_, week_;      // per shift
    mutable std::vector<char> worked_;          // scratch, zeroed after use
    mutable std::vector<std::int32_t> weekDays_;  // scratch, zeroed after use
    mutable std::vector<char> movable_;         // scratch: week has a movable shift
    std::int32_t weeks_ = 0;
    std::int32_t horizon_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_MIN_WEEKLY_REST_DAYS_COST_HPP

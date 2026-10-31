#ifndef ES_CONSTRAINTS_MAX_WEEKLY_MINUTES_COST_HPP
#define ES_CONSTRAINTS_MAX_WEEKLY_MINUTES_COST_HPP

#include "es/constraints/weekly_minutes_constraint.hpp"

namespace es {

// Hard, weight 1: maxWeeklyMinutesCost. Raw cost per employee = the
// assigned minutes beyond `minutes` (default 2400 = a 40-hour week) in each
// calendar week, so 0 = no week is over the cap. weekAnchor aligns the weeks
// to a calendar boundary (an engine day that starts a week); 0 =
// day-0-anchored blocks. A week holding only pinned shifts is skipped.
struct MaxWeeklyMinutesCost final : WeeklyMinutesConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxWeeklyMinutesCost(double weight = kDefaultWeight, Minutes minutes = 40 * 60,
                                  int weekAnchor = 0)
        : WeeklyMinutesConstraint("maxWeeklyMinutesCost", weight, kDefaultLevel, minutes,
                                  weekAnchor) {}
    double weekCost(std::int64_t t) const override {
        return t > threshold_ ? static_cast<double>(t - threshold_) : 0.0;
    }
};

}  // namespace es

#endif  // ES_CONSTRAINTS_MAX_WEEKLY_MINUTES_COST_HPP

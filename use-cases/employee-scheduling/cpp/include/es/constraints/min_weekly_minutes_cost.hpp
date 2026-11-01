#ifndef ES_CONSTRAINTS_MIN_WEEKLY_MINUTES_COST_HPP
#define ES_CONSTRAINTS_MIN_WEEKLY_MINUTES_COST_HPP

#include "es/constraints/weekly_minutes_constraint.hpp"

namespace es {

// Soft, weight 1: minWeeklyMinutesCost. Raw cost per employee = the
// shortfall in minutes below `minutes` (default 480) in EVERY calendar week of
// the horizon (guaranteed-hours semantics — an idle week costs the full
// threshold), so 0 = every week reaches it. weekAnchor aligns the weeks as in
// MaxWeeklyMinutesCost. A week holding only pinned shifts is skipped.
struct MinWeeklyMinutesCost final : WeeklyMinutesConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit MinWeeklyMinutesCost(double weight = kDefaultWeight, Minutes minutes = 8 * 60,
                                  int weekAnchor = 0)
        : WeeklyMinutesConstraint("minWeeklyMinutesCost", weight, kDefaultLevel, minutes,
                                  weekAnchor) {}
    double weekCost(std::int64_t t) const override {
        return t < threshold_ ? static_cast<double>(threshold_ - t) : 0.0;
    }
};

}  // namespace es

#endif  // ES_CONSTRAINTS_MIN_WEEKLY_MINUTES_COST_HPP

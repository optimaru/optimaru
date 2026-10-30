#ifndef ES_CONSTRAINTS_MAX_WEEKLY_MINUTES_HPP
#define ES_CONSTRAINTS_MAX_WEEKLY_MINUTES_HPP

#include "es/constraints/weekly_minutes_constraint.hpp"

namespace es {

// Hard (default): assigned minutes beyond `minutes` in any calendar week
// (e.g. 2400 = a 40-hour week). weekAnchor aligns the weeks to a calendar
// boundary (an engine day that starts a week); 0 = day-0-anchored blocks.
struct MaxWeeklyMinutes final : WeeklyMinutesConstraint {
    explicit MaxWeeklyMinutes(double weight = 1.0, Minutes minutes = 40 * 60, int weekAnchor = 0)
        : WeeklyMinutesConstraint("Max weekly minutes", weight, /*hard=*/true, minutes, weekAnchor) {}
    double weekCost(std::int64_t t) const override {
        return t > threshold_ ? static_cast<double>(t - threshold_) : 0.0;
    }
};

}  // namespace es

#endif  // ES_CONSTRAINTS_MAX_WEEKLY_MINUTES_HPP

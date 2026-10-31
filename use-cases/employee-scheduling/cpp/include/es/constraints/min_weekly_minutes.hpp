#ifndef ES_CONSTRAINTS_MIN_WEEKLY_MINUTES_HPP
#define ES_CONSTRAINTS_MIN_WEEKLY_MINUTES_HPP

#include "es/constraints/weekly_minutes_constraint.hpp"

namespace es {

// Soft (default): shortfall below `minutes` in EVERY calendar week of the
// horizon (guaranteed-hours semantics — an idle week costs the full
// threshold).
struct MinWeeklyMinutes final : WeeklyMinutesConstraint {
    explicit MinWeeklyMinutes(double weight = 1.0, Minutes minutes = 8 * 60, int weekAnchor = 0)
        : WeeklyMinutesConstraint("Min weekly minutes", weight, /*hard=*/false, minutes, weekAnchor) {}
    double weekCost(std::int64_t t) const override {
        return t < threshold_ ? static_cast<double>(threshold_ - t) : 0.0;
    }
};

}  // namespace es

#endif  // ES_CONSTRAINTS_MIN_WEEKLY_MINUTES_HPP

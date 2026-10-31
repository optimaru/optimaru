#ifndef ES_CONSTRAINTS_MIN_CONSECUTIVE_DAYS_HPP
#define ES_CONSTRAINTS_MIN_CONSECUTIVE_DAYS_HPP

#include "es/constraints/day_presence_constraint.hpp"

namespace es {

// Soft (default): shortfall below `value` of any (non-empty) run of
// consecutive worked days ("if you work, work at least `value` days in a
// row").
struct MinConsecutiveDays final : DayPresenceConstraint {
    explicit MinConsecutiveDays(double weight = 1.0, int value = 2)
        : DayPresenceConstraint("Min consecutive working days", weight, /*hard=*/false),
          value_(value) {}
    double runCost(std::int32_t len, bool touchesEnd) const override {
        if (touchesEnd) return 0.0;  // C: a run cut by the horizon end may continue past it
        return len < value_ ? static_cast<double>(value_ - len) : 0.0;
    }

private:
    std::int32_t value_;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_MIN_CONSECUTIVE_DAYS_HPP

#ifndef ES_CONSTRAINTS_MAX_CONSECUTIVE_DAYS_HPP
#define ES_CONSTRAINTS_MAX_CONSECUTIVE_DAYS_HPP

#include "es/constraints/day_presence_constraint.hpp"

namespace es {

// Hard (default): days beyond `value` in any run of consecutive worked days
// (e.g. value=6: "at most 6 days in a row").
struct MaxConsecutiveDays final : DayPresenceConstraint {
    explicit MaxConsecutiveDays(double weight = 1.0, int value = 6)
        : DayPresenceConstraint("Max consecutive working days", weight, /*hard=*/true),
          value_(value) {}
    double runCost(std::int32_t len, bool /*touchesEnd*/) const override {
        return len > value_ ? static_cast<double>(len - value_) : 0.0;  // excess always counts
    }

private:
    std::int32_t value_;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_MAX_CONSECUTIVE_DAYS_HPP

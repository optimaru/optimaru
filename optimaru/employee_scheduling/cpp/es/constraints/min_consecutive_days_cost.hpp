#ifndef ES_CONSTRAINTS_MIN_CONSECUTIVE_DAYS_COST_HPP
#define ES_CONSTRAINTS_MIN_CONSECUTIVE_DAYS_COST_HPP

#include "es/constraints/day_presence_constraint.hpp"

namespace es {

// Soft, weight 1: minConsecutiveDaysCost. Raw cost per employee = the
// shortfall below `value` (default 2) of each maximal run of consecutive
// worked days ("if you work, work at least `value` days in a row"); a run
// reaching the last day of the horizon and a pinned-only run are waived. 0 =
// every run is at least `value` days long.
struct MinConsecutiveDaysCost final : DayPresenceConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit MinConsecutiveDaysCost(double weight = kDefaultWeight, int value = 2)
        : DayPresenceConstraint("minConsecutiveDaysCost", weight, kDefaultLevel), value_(value) {}
    double runCost(std::int32_t len, bool touchesEnd) const override {
        if (touchesEnd) return 0.0;  // C: a run cut by the horizon end may continue past it
        return len < value_ ? static_cast<double>(value_ - len) : 0.0;
    }

private:
    std::int32_t value_;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_MIN_CONSECUTIVE_DAYS_COST_HPP

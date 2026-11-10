#ifndef ES_CONSTRAINTS_MAX_CONSECUTIVE_DAYS_COST_HPP
#define ES_CONSTRAINTS_MAX_CONSECUTIVE_DAYS_COST_HPP

#include "es/constraints/day_presence_constraint.hpp"

namespace es {

// Hard, weight 1: maxConsecutiveDaysCost. Raw cost per employee = the days
// beyond `value` (default 6) in each maximal run of consecutive worked days
// (value=6: "at most 6 days in a row"; a run of 8 costs 2). A day counts once
// even with several shifts; a run made only of pinned days is skipped. 0 = no
// run is longer than `value`.
struct MaxConsecutiveDaysCost final : DayPresenceConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxConsecutiveDaysCost(double weight = kDefaultWeight, int value = 6)
        : DayPresenceConstraint("maxConsecutiveDaysCost", weight, kDefaultLevel),
          value_(value) {}
    double runCost(std::int32_t len, bool /*touchesEnd*/) const override {
        return len > value_ ? static_cast<double>(len - value_) : 0.0;  // excess always counts
    }

private:
    std::int32_t value_;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_MAX_CONSECUTIVE_DAYS_COST_HPP

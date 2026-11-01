#ifndef ES_CONSTRAINTS_DESIRED_DAY_COST_HPP
#define ES_CONSTRAINTS_DESIRED_DAY_COST_HPP

#include "es/constraints/date_overlap_constraint.hpp"

namespace es {

// Soft, weight 1, REWARD: desiredDayCost. Raw cost per employee = MINUS the
// minutes of its assigned, non-pinned shifts on its desired days, or with
// `perDay` minus 1 per such shift: raw <= 0, and 0 = no desired day is
// worked (nothing earned). A reward can never be hard (reward = true,
// levelFlippable = false: hard() / level='hard' throw).
struct DesiredDayCost final : DateOverlapConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit DesiredDayCost(double weight = kDefaultWeight, bool perDay = false)
        : DateOverlapConstraint("desiredDayCost", weight, kDefaultLevel,
                                &Employee::desiredDates, -1.0, perDay) {
        reward = true;
        levelFlippable = false;
    }
};

}  // namespace es

#endif  // ES_CONSTRAINTS_DESIRED_DAY_COST_HPP

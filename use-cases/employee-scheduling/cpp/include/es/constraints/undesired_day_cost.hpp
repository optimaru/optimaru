#ifndef ES_CONSTRAINTS_UNDESIRED_DAY_COST_HPP
#define ES_CONSTRAINTS_UNDESIRED_DAY_COST_HPP

#include "es/constraints/date_overlap_constraint.hpp"

namespace es {

// Soft, weight 1: undesiredDayCost. Raw cost per employee = the minutes of
// its assigned, non-pinned shifts on its undesired days, or with `perDay` 1
// per such shift, so 0 = nobody works on a day they asked to avoid.
struct UndesiredDayCost final : DateOverlapConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit UndesiredDayCost(double weight = kDefaultWeight, bool perDay = false)
        : DateOverlapConstraint("undesiredDayCost", weight, kDefaultLevel,
                                &Employee::undesiredDates, +1.0, perDay) {}
};

}  // namespace es

#endif  // ES_CONSTRAINTS_UNDESIRED_DAY_COST_HPP

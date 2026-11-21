#ifndef ES_CONSTRAINTS_EMPLOYEE_UNAVAILABILITY_COST_HPP
#define ES_CONSTRAINTS_EMPLOYEE_UNAVAILABILITY_COST_HPP

#include "es/constraints/date_overlap_constraint.hpp"

namespace es {

// Hard, weight 1: employeeUnavailabilityCost. Raw cost per employee = the
// minutes of its assigned, non-pinned shifts that fall on its unavailable
// days (ScoreCalculator::overlapWithDates), so 0 = nobody works on a day they
// are unavailable.
struct EmployeeUnavailabilityCost final : DateOverlapConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit EmployeeUnavailabilityCost(double weight = kDefaultWeight)
        : DateOverlapConstraint("employeeUnavailabilityCost", weight, kDefaultLevel,
                                &Employee::unavailableDates, +1.0) {}
};

}  // namespace es

#endif  // ES_CONSTRAINTS_EMPLOYEE_UNAVAILABILITY_COST_HPP

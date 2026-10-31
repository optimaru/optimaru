#ifndef ES_CONSTRAINTS_UNAVAILABLE_EMPLOYEE_HPP
#define ES_CONSTRAINTS_UNAVAILABLE_EMPLOYEE_HPP

#include "es/constraints/date_overlap_constraint.hpp"

namespace es {

// Hard (default): minutes of assigned shifts overlapping the employee's unavailable days.
struct UnavailableEmployee final : DateOverlapConstraint {
    explicit UnavailableEmployee(double weight = 1.0)
        : DateOverlapConstraint("Unavailable employee", weight, /*hard=*/true,
                                &Employee::unavailableDates, +1.0) {}
};

}  // namespace es

#endif  // ES_CONSTRAINTS_UNAVAILABLE_EMPLOYEE_HPP

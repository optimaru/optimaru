#ifndef ES_CONSTRAINTS_DESIRED_DAY_HPP
#define ES_CONSTRAINTS_DESIRED_DAY_HPP

#include "es/constraints/date_overlap_constraint.hpp"

namespace es {

// Soft (default), reward (negative cost): assigned shifts on the employee's
// desired days — overlap minutes, or a per-day count if perDay.
struct DesiredDay final : DateOverlapConstraint {
    explicit DesiredDay(double weight = 1.0, bool perDay = false)
        : DateOverlapConstraint("Desired day for employee", weight, /*hard=*/false,
                                &Employee::desiredDates, -1.0, perDay) {}
};

}  // namespace es

#endif  // ES_CONSTRAINTS_DESIRED_DAY_HPP

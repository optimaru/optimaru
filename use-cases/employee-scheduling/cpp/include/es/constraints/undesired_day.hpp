#ifndef ES_CONSTRAINTS_UNDESIRED_DAY_HPP
#define ES_CONSTRAINTS_UNDESIRED_DAY_HPP

#include "es/constraints/date_overlap_constraint.hpp"

namespace es {

// Soft (default), penalty: assigned shifts on the employee's undesired days —
// overlap minutes, or a per-day count (1 per undesired day worked) if perDay.
struct UndesiredDay final : DateOverlapConstraint {
    explicit UndesiredDay(double weight = 1.0, bool perDay = false)
        : DateOverlapConstraint("Undesired day for employee", weight, /*hard=*/false,
                                &Employee::undesiredDates, +1.0, perDay) {}
};

}  // namespace es

#endif  // ES_CONSTRAINTS_UNDESIRED_DAY_HPP

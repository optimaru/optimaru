#ifndef ES_CONSTRAINTS_HPP
#define ES_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases) and carries the pattern's documentation. The
// family bases live with the machinery they share: the assignment family with
// its compiled delta group in assignment_delta_group.hpp, the day-presence /
// weekly-minutes / location-presence bases each in their own file next to the
// components deriving from them.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "es/constraints/common.hpp"
#include "es/constraints/assignment_cost.hpp"
#include "es/constraints/assignment_constraint.hpp"
#include "es/constraints/assignment_delta_group.hpp"
#include "es/constraints/balance_location.hpp"
#include "es/constraints/balance_min_max_gap.hpp"
#include "es/constraints/balance_shift_assignments.hpp"
#include "es/constraints/balance_weekly_minutes_gap.hpp"
#include "es/constraints/date_overlap_constraint.hpp"
#include "es/constraints/day_pattern.hpp"
#include "es/constraints/day_presence_constraint.hpp"
#include "es/constraints/desired_day.hpp"
#include "es/constraints/desired_shift.hpp"
#include "es/constraints/location_presence_base.hpp"
#include "es/constraints/location_run.hpp"
#include "es/constraints/location_run_gap.hpp"
#include "es/constraints/max_consecutive_days.hpp"
#include "es/constraints/max_consecutive_off.hpp"
#include "es/constraints/max_one_shift_per_day.hpp"
#include "es/constraints/max_weekly_minutes.hpp"
#include "es/constraints/min_consecutive_days.hpp"
#include "es/constraints/min_weekly_minutes.hpp"
#include "es/constraints/min_weekly_rest_days.hpp"
#include "es/constraints/minimum_rest.hpp"
#include "es/constraints/overlapping_shift.hpp"
#include "es/constraints/required_skill.hpp"
#include "es/constraints/rest_after_consecutive_work.hpp"
#include "es/constraints/rest_after_location_run.hpp"
#include "es/constraints/scoped_unit.hpp"
#include "es/constraints/shift_coverage.hpp"
#include "es/constraints/target_days_off.hpp"
#include "es/constraints/unassigned_shift.hpp"
#include "es/constraints/unavailable_employee.hpp"
#include "es/constraints/undesired_day.hpp"
#include "es/constraints/weekly_minutes_constraint.hpp"
#include "es/constraints/worker_day_count_in_range.hpp"
#include "es/constraints/worker_location_count.hpp"
#include "es/constraints/worker_pair_same_shift.hpp"

namespace es {

// The default assembly: all eight constraints at weight 1 — equivalent to the
// monolithic EmployeeSchedulingIncrementalScoreCalculator / ScoreCalculator.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly, so
// a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints().weight("Undesired day for employee", 5).into(calc);
inline ConstraintAssembly defaultConstraints() {
    ConstraintAssembly assembly;
    assembly.emplace<RequiredSkill>();
    assembly.emplace<UnavailableEmployee>();
    assembly.emplace<OverlappingShift>();
    assembly.emplace<MaxOneShiftPerDay>();
    assembly.emplace<MinimumRest>();
    assembly.emplace<UndesiredDay>();
    assembly.emplace<DesiredDay>();
    assembly.emplace<BalanceShiftAssignments>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator& calc) {
    defaultConstraints().into(calc);
}

}  // namespace es

#endif  // ES_CONSTRAINTS_HPP

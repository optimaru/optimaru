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
// to change. Adding a component is a file plus a line in catalogConstraints()
// below (and in defaultConstraints() when it belongs to the default assembly),
// plus its entry in the rule factory of python/bindings.cpp.

#include "es/constraints/common.hpp"
#include "es/constraints/assignment_constraint.hpp"
#include "es/constraints/assignment_cost.hpp"
#include "es/constraints/assignment_delta_group.hpp"
#include "es/constraints/date_overlap_constraint.hpp"
#include "es/constraints/day_pattern_cost.hpp"
#include "es/constraints/day_presence_constraint.hpp"
#include "es/constraints/desired_day_cost.hpp"
#include "es/constraints/desired_shift_cost.hpp"
#include "es/constraints/employee_unavailability_cost.hpp"
#include "es/constraints/location_imbalance_cost.hpp"
#include "es/constraints/location_presence_base.hpp"
#include "es/constraints/location_run_gap_cost.hpp"
#include "es/constraints/location_run_length_cost.hpp"
#include "es/constraints/max_consecutive_days_cost.hpp"
#include "es/constraints/max_consecutive_days_off_cost.hpp"
#include "es/constraints/max_one_shift_per_day_cost.hpp"
#include "es/constraints/max_weekly_minutes_cost.hpp"
#include "es/constraints/min_consecutive_days_cost.hpp"
#include "es/constraints/min_rest_cost.hpp"
#include "es/constraints/min_weekly_minutes_cost.hpp"
#include "es/constraints/min_weekly_rest_days_cost.hpp"
#include "es/constraints/required_skill_cost.hpp"
#include "es/constraints/rest_after_consecutive_work_cost.hpp"
#include "es/constraints/rest_after_location_run_cost.hpp"
#include "es/constraints/scoped_unit.hpp"
#include "es/constraints/shift_count_gap_cost.hpp"
#include "es/constraints/shift_count_imbalance_cost.hpp"
#include "es/constraints/shift_coverage_cost.hpp"
#include "es/constraints/shift_overlap_cost.hpp"
#include "es/constraints/target_days_off_cost.hpp"
#include "es/constraints/unassigned_shifts_cost.hpp"
#include "es/constraints/undesired_day_cost.hpp"
#include "es/constraints/weekly_minutes_constraint.hpp"
#include "es/constraints/weekly_minutes_gap_cost.hpp"
#include "es/constraints/worker_day_count_in_range_cost.hpp"
#include "es/constraints/worker_location_count_cost.hpp"
#include "es/constraints/worker_pairing_cost.hpp"

namespace es {

// The default assembly: the eight default constraints at their kDefaultWeight
// (all 1) and kDefaultLevel — equivalent to the monolithic
// EmployeeSchedulingIncrementalScoreCalculator / ScoreCalculator.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly, so
// a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints().weight("undesiredDayCost", 5).soft("minRestCost").into(calc);
inline ConstraintAssembly defaultConstraints() {
    ConstraintAssembly assembly;
    assembly.emplace<RequiredSkillCost>();
    assembly.emplace<EmployeeUnavailabilityCost>();
    assembly.emplace<ShiftOverlapCost>();
    assembly.emplace<MaxOneShiftPerDayCost>();
    assembly.emplace<MinRestCost>();
    assembly.emplace<UndesiredDayCost>();
    assembly.emplace<DesiredDayCost>();
    assembly.emplace<ShiftCountImbalanceCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator& calc) {
    defaultConstraints().into(calc);
}

// Every component the rule factory (python/bindings.cpp addConstraintByName)
// can build, once each, at its default parameters and kDefaultWeight /
// kDefaultLevel, in factory order (= CONSTRAINT_NAMES): the catalog behind
// constraint_info() (optimaru::catalogInfo(defaultConstraints(),
// catalogConstraints()) = the eight defaults first, then the 22 rule-only
// constraints with defaultEnabled = false; ConstraintAssembly::info() lists
// unit constraints before global ones, so those 22 come as 16 units, then 6
// globals). The defaults are restated so this reads as the complete list;
// catalogInfo checks that both assemblies declare them identically. A new
// component is a line here AND a factory entry.
inline ConstraintAssembly catalogConstraints() {
    ConstraintAssembly assembly;
    assembly.emplace<RequiredSkillCost>();
    assembly.emplace<EmployeeUnavailabilityCost>();
    assembly.emplace<ShiftOverlapCost>();
    assembly.emplace<MaxOneShiftPerDayCost>();
    assembly.emplace<MinRestCost>();
    assembly.emplace<UndesiredDayCost>();
    assembly.emplace<DesiredDayCost>();
    assembly.emplace<DesiredShiftCost>();
    assembly.emplace<ShiftCountImbalanceCost>();
    assembly.emplace<MaxConsecutiveDaysCost>();
    assembly.emplace<MinConsecutiveDaysCost>();
    assembly.emplace<MaxConsecutiveDaysOffCost>();
    assembly.emplace<RestAfterConsecutiveWorkCost>();
    assembly.emplace<MaxWeeklyMinutesCost>();
    assembly.emplace<MinWeeklyMinutesCost>();
    assembly.emplace<MinWeeklyRestDaysCost>();
    assembly.emplace<AssignmentCost>();
    assembly.emplace<UnassignedShiftsCost>();
    assembly.emplace<ShiftCountGapCost>();
    assembly.emplace<WeeklyMinutesGapCost>();
    assembly.emplace<ShiftCoverageCost>();
    assembly.emplace<LocationRunLengthCost>();
    assembly.emplace<RestAfterLocationRunCost>();
    assembly.emplace<LocationRunGapCost>();
    assembly.emplace<WorkerLocationCountCost>();
    assembly.emplace<DayPatternCost>();
    assembly.emplace<TargetDaysOffCost>();
    assembly.emplace<WorkerDayCountInRangeCost>();
    assembly.emplace<LocationImbalanceCost>();
    assembly.emplace<WorkerPairingCost>();
    return assembly;
}

}  // namespace es

#endif  // ES_CONSTRAINTS_HPP

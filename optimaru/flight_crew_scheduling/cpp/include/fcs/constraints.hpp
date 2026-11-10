#ifndef FCS_CONSTRAINTS_HPP
#define FCS_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (RequiredSkillCost -> constraints/required_skill_cost.hpp); common.hpp holds
// what all of them need (the calculator aliases, the per-assignment base and
// the itinerary base) and carries the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// defaultConstraints below when it belongs to the default assembly).

#include "fcs/constraints/common.hpp"
#include "fcs/constraints/employee_unavailability_cost.hpp"
#include "fcs/constraints/first_assignment_not_departing_from_home_cost.hpp"
#include "fcs/constraints/flight_conflict_cost.hpp"
#include "fcs/constraints/last_assignment_not_arriving_at_home_cost.hpp"
#include "fcs/constraints/required_skill_cost.hpp"
#include "fcs/constraints/transfer_between_two_flights_cost.hpp"

namespace fcs {

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full constraint set, every component at its own defaults:
//
//   requiredSkillCost                         hard, weight 100
//   flightConflictCost                        hard, weight 10
//   transferBetweenTwoFlightsCost             hard, weight 1
//   employeeUnavailabilityCost                hard, weight 10
//   firstAssignmentNotDepartingFromHomeCost   soft, weight 1000
//   lastAssignmentNotArrivingAtHomeCost       soft, weight 1000
//
// This assembly scores bit-for-bit like ScoreCalculator::score and
// the monolithic incremental calculator (both read the same kDefaultWeight
// constants). It is also the domain's constraint catalog: constraint_info()
// reads the defaults off it, so they live in the component constructors and
// nowhere else.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("employeeUnavailabilityCost", 2)
//         .soft("transferBetweenTwoFlightsCost")
//         .drop("firstAssignmentNotDepartingFromHomeCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints() {
    ConstraintAssembly assembly;
    assembly.emplace<RequiredSkillCost>();
    assembly.emplace<FlightConflictCost>();
    assembly.emplace<TransferBetweenTwoFlightsCost>();
    assembly.emplace<EmployeeUnavailabilityCost>();
    assembly.emplace<FirstAssignmentNotDepartingFromHomeCost>();
    assembly.emplace<LastAssignmentNotArrivingAtHomeCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator& calculator) {
    defaultConstraints().into(calculator);
}

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_HPP

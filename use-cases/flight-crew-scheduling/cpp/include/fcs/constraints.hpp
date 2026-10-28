#ifndef FCS_CONSTRAINTS_HPP
#define FCS_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases, the per-assignment base and the itinerary
// base) and carries the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "fcs/constraints/common.hpp"
#include "fcs/constraints/employee_unavailability.hpp"
#include "fcs/constraints/first_assignment_not_departing_from_home.hpp"
#include "fcs/constraints/flight_conflict.hpp"
#include "fcs/constraints/last_assignment_not_arriving_at_home.hpp"
#include "fcs/constraints/required_skill.hpp"
#include "fcs/constraints/transfer_between_two_flights.hpp"

namespace fcs
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider at the quickstart's weights. This assembly scores
// bit-for-bit like ScoreCalculator::calculateScore and the monolithic
// incremental calculator.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("employeeUnavailability", 2)
//         .drop("firstAssignmentNotDepartingFromHome")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<RequiredSkill>();
    assembly.emplace<FlightConflict>();
    assembly.emplace<TransferBetweenTwoFlights>();
    assembly.emplace<EmployeeUnavailability>();
    assembly.emplace<FirstAssignmentNotDepartingFromHome>();
    assembly.emplace<LastAssignmentNotArrivingAtHome>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_HPP

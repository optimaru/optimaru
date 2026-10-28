#ifndef TS_CONSTRAINTS_HPP
#define TS_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases and the global-constraint base) and carries the
// pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "ts/constraints/common.hpp"
#include "ts/constraints/evenly_confrontation_count.hpp"
#include "ts/constraints/fair_assignment_count_per_team.hpp"
#include "ts/constraints/one_assignment_per_date_per_team.hpp"
#include "ts/constraints/unavailability_penalty.hpp"

namespace ts
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider at the quickstart's level weights. This assembly scores
// bit-for-bit like ScoreCalculator::calculateScore and the monolithic
// incremental calculator — both unfairness values go through the same
// ScoreCalculator::unfairnessFromAggregates, and the medium level is folded at
// the same kMediumScale.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("evenlyConfrontationCount", 2)
//         .drop("fairAssignmentCountPerTeam")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<OneAssignmentPerDatePerTeam>();
    assembly.emplace<UnavailabilityPenaltyConstraint>();
    assembly.emplace<FairAssignmentCountPerTeam>();
    assembly.emplace<EvenlyConfrontationCount>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace ts

#endif  // TS_CONSTRAINTS_HPP

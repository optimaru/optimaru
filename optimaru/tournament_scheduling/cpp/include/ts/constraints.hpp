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

#include "ts/constraints/assignment_count_imbalance_cost.hpp"
#include "ts/constraints/common.hpp"
#include "ts/constraints/confrontation_count_imbalance_cost.hpp"
#include "ts/constraints/team_conflict_cost.hpp"
#include "ts/constraints/team_unavailability_cost.hpp"

namespace ts {

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full set at the components' default weights and levels
// (kDefaultWeight / kDefaultLevel). This assembly scores bit-for-bit like
// ScoreCalculator::score and the monolithic incremental calculator —
// both unfairness values go through the same
// ScoreCalculator::unfairnessFromAggregates, and the medium level is folded
// with the same kMediumFold (domain.hpp).
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("confrontationCountImbalanceCost", 2)
//         .drop("assignmentCountImbalanceCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints() {
    ConstraintAssembly assembly;
    assembly.emplace<TeamConflictCost>();
    assembly.emplace<TeamUnavailabilityCost>();
    assembly.emplace<AssignmentCountImbalanceCost>();
    assembly.emplace<ConfrontationCountImbalanceCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator& calculator) {
    defaultConstraints().into(calculator);
}

}  // namespace ts

#endif  // TS_CONSTRAINTS_HPP

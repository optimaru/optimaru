#ifndef FP_CONSTRAINTS_HPP
#define FP_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases and the line chain walk) and carries the
// pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "fp/constraints/common.hpp"
#include "fp/constraints/ideal_end_date_time.hpp"
#include "fp/constraints/max_end_date_time.hpp"
#include "fp/constraints/maximize_jobs_assigned.hpp"
#include "fp/constraints/minimize_makespan.hpp"
#include "fp/constraints/operator_cleaning_conflict.hpp"

namespace fp
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider at the quickstart's level weights. This assembly scores
// bit-for-bit like ScoreCalculator::calculateScore and the monolithic
// incremental calculator.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("idealEndDateTime", 2)
//         .drop("maxEndDateTime")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<MaxEndDateTime>();
    assembly.emplace<OperatorCleaningConflict>();
    assembly.emplace<IdealEndDateTime>();
    assembly.emplace<MaximizeJobsAssigned>();
    assembly.emplace<MinimizeMakespan>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace fp

#endif  // FP_CONSTRAINTS_HPP

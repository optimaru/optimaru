#ifndef FP_CONSTRAINTS_HPP
#define FP_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (MaxEndTimeCost -> constraints/max_end_time_cost.hpp); common.hpp holds what
// all of them need (the calculator aliases and the line chain walk) and
// carries the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// defaultConstraints below when it belongs to the default assembly).

#include "fp/constraints/after_ideal_end_time_cost.hpp"
#include "fp/constraints/common.hpp"
#include "fp/constraints/max_end_time_cost.hpp"
#include "fp/constraints/operator_cleaning_conflict_cost.hpp"
#include "fp/constraints/squared_makespan_cost.hpp"
#include "fp/constraints/unassigned_jobs_cost.hpp"

namespace fp
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider, every component at its own defaults:
//
//   maxEndTimeCost                 hard,   weight 1   (unit: line)
//   operatorCleaningConflictCost   hard,   weight 1   (global)
//   afterIdealEndTimeCost          medium, weight 1   (unit: line)
//   unassignedJobsCost             medium, weight 1   (global)
//   squaredMakespanCost            soft,   weight 1   (unit: line)
//
// The medium weights are in medium units; the domain's kMediumFold (10^10,
// domain.hpp) folds them into the engine's soft level. This assembly scores
// bit-for-bit like ScoreCalculator::calculateScore and the monolithic
// incremental calculator (both read the same kDefaultWeight constants). It is
// also the domain's constraint catalog: constraint_info() reads the defaults
// off it, so they live in the component constructors and nowhere else.
// list_constraints() / constraint_info() report the unit constraints first,
// then the globals (max_end_time_cost, after_ideal_end_time_cost,
// squared_makespan_cost, operator_cleaning_conflict_cost,
// unassigned_jobs_cost).
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("afterIdealEndTimeCost", 2)
//         .drop("maxEndTimeCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<MaxEndTimeCost>();
    assembly.emplace<OperatorCleaningConflictCost>();
    assembly.emplace<AfterIdealEndTimeCost>();
    assembly.emplace<UnassignedJobsCost>();
    assembly.emplace<SquaredMakespanCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace fp

#endif  // FP_CONSTRAINTS_HPP

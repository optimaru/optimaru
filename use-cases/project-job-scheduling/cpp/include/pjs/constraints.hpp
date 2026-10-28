#ifndef PJS_CONSTRAINTS_HPP
#define PJS_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases, the date-chain derivation and the
// whole-schedule global base) and carries the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "pjs/constraints/common.hpp"
#include "pjs/constraints/non_renewable_resource_capacity.hpp"
#include "pjs/constraints/renewable_resource_capacity.hpp"
#include "pjs/constraints/total_makespan.hpp"
#include "pjs/constraints/total_project_delay.hpp"

namespace pjs
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
//         .weight("nonRenewableResourceCapacity", 2)
//         .drop("renewableResourceCapacity")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<NonRenewableResourceCapacity>();
    assembly.emplace<RenewableResourceCapacity>();
    assembly.emplace<TotalProjectDelay>();
    assembly.emplace<TotalMakespan>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace pjs

#endif  // PJS_CONSTRAINTS_HPP

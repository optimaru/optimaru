#ifndef MNT_CONSTRAINTS_HPP
#define MNT_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases, the per-job base and the day-overlap global
// base) and carries the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "mnt/constraints/after_ideal_end_date.hpp"
#include "mnt/constraints/before_ideal_end_date.hpp"
#include "mnt/constraints/common.hpp"
#include "mnt/constraints/crew_conflict.hpp"
#include "mnt/constraints/max_end_date.hpp"
#include "mnt/constraints/min_start_date.hpp"
#include "mnt/constraints/tag_conflict.hpp"

namespace mnt
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
//         .weight("afterIdealEndDate", 2)
//         .drop("beforeIdealEndDate")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<CrewConflict>();
    assembly.emplace<MinStartDate>();
    assembly.emplace<MaxEndDate>();
    assembly.emplace<BeforeIdealEndDate>();
    assembly.emplace<AfterIdealEndDate>();
    assembly.emplace<TagConflict>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_HPP

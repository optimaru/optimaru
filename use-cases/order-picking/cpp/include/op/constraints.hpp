#ifndef OP_CONSTRAINTS_HPP
#define OP_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases) and carries the pattern's documentation.
//
// Adding a component is a file plus an include here (and an `emplace` in
// defaultConstraints() below when it belongs to the default assembly).

#include "op/constraints/common.hpp"
#include "op/constraints/minimize_distance_from_last_pick_task_to_path_origin.hpp"
#include "op/constraints/minimize_distance_from_previous_pick_task.hpp"
#include "op/constraints/minimize_order_split_by_trolley.hpp"
#include "op/constraints/required_number_of_buckets.hpp"

namespace op
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
//         .weight("minimizeDistanceFromLastPickTaskToPathOrigin", 2)
//         .drop("minimizeDistanceFromPreviousPickTask")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<RequiredNumberOfBuckets>();
    assembly.emplace<MinimizeDistanceFromPreviousPickTask>();
    assembly.emplace<MinimizeDistanceFromLastPickTaskToPathOrigin>();
    assembly.emplace<MinimizeOrderSplitByTrolley>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace op

#endif  // OP_CONSTRAINTS_HPP

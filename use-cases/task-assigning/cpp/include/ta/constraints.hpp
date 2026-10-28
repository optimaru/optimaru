#ifndef TA_CONSTRAINTS_HPP
#define TA_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases, the chain walk and the priority-family base)
// and carries the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "ta/constraints/common.hpp"
#include "ta/constraints/critical_priority_task_end_time.hpp"
#include "ta/constraints/major_priority_task_end_time.hpp"
#include "ta/constraints/minimize_makespan.hpp"
#include "ta/constraints/minimize_unassigned_tasks.hpp"
#include "ta/constraints/minor_priority_task_end_time.hpp"
#include "ta/constraints/no_missing_skills.hpp"

namespace ta
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider at the quickstart's Bendable level weights. This assembly
// scores bit-for-bit like ScoreCalculator::calculateScore and the monolithic
// incremental calculator.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("criticalPriorityTaskEndTime", 2)
//         .drop("majorPriorityTaskEndTime")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<NoMissingSkills>();
    assembly.emplace<MinimizeUnassignedTasks>();
    assembly.emplace<MinimizeMakespan>();
    assembly.emplace<CriticalPriorityTaskEndTime>();
    assembly.emplace<MajorPriorityTaskEndTime>();
    assembly.emplace<MinorPriorityTaskEndTime>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace ta

#endif  // TA_CONSTRAINTS_HPP

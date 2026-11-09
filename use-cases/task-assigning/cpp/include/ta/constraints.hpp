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

#include <cstdint>

#include "ta/constraints/common.hpp"
#include "ta/constraints/critical_priority_task_end_time_cost.hpp"
#include "ta/constraints/major_priority_task_end_time_cost.hpp"
#include "ta/constraints/minor_priority_task_end_time_cost.hpp"
#include "ta/constraints/required_skill_cost.hpp"
#include "ta/constraints/squared_makespan_cost.hpp"
#include "ta/constraints/unassigned_tasks_cost.hpp"

namespace ta
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider at the components' default weights and levels (each
// component's kDefaultWeight / kDefaultLevel; constraint_info() reads them off
// this assembly). It scores bit-for-bit like ScoreCalculator::score
// and the monolithic incremental calculator. The calculator scores the unit
// constraints first and the global unassignedTasksCost after them, which is
// the list_constraints() order.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("criticalPriorityTaskEndTimeCost", 2)
//         .drop("majorPriorityTaskEndTimeCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<RequiredSkillCost>();
    assembly.emplace<UnassignedTasksCost>();
    assembly.emplace<SquaredMakespanCost>();
    assembly.emplace<CriticalPriorityTaskEndTimeCost>();
    assembly.emplace<MajorPriorityTaskEndTimeCost>();
    assembly.emplace<MinorPriorityTaskEndTimeCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

// The soft[2] end-time weight of each priority in the monolithic calculators:
// the three priority components' kDefaultWeight (x 4 / x 2 / x 1), read here
// so the monoliths never re-type them.
inline std::int64_t priorityWeight(Priority priority)
{
    constexpr auto kCritical
        = static_cast<std::int64_t>(CriticalPriorityTaskEndTimeCost::kDefaultWeight);
    constexpr auto kMajor
        = static_cast<std::int64_t>(MajorPriorityTaskEndTimeCost::kDefaultWeight);
    constexpr auto kMinor
        = static_cast<std::int64_t>(MinorPriorityTaskEndTimeCost::kDefaultWeight);
    static_assert(kCritical == CriticalPriorityTaskEndTimeCost::kDefaultWeight
                      && kMajor == MajorPriorityTaskEndTimeCost::kDefaultWeight
                      && kMinor == MinorPriorityTaskEndTimeCost::kDefaultWeight,
                  "the monolithic calculators assume integral priority weights");
    switch (priority)
    {
    case Priority::CRITICAL:
        return kCritical;
    case Priority::MAJOR:
        return kMajor;
    case Priority::MINOR:
    default:
        return kMinor;
    }
}

}  // namespace ta

#endif  // TA_CONSTRAINTS_HPP

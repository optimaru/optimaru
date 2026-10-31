#ifndef MNT_CONSTRAINTS_HPP
#define MNT_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (CrewConflictCost -> constraints/crew_conflict_cost.hpp); common.hpp holds
// what all of them need (the calculator aliases, the per-job base and the
// day-overlap global base) and carries the pattern's documentation.
//
// Adding a component is a file plus an include here (and an `emplace` in
// defaultConstraints() below when it belongs to the default assembly).

#include "mnt/constraints/after_ideal_end_date_cost.hpp"
#include "mnt/constraints/before_ideal_end_date_cost.hpp"
#include "mnt/constraints/common.hpp"
#include "mnt/constraints/crew_conflict_cost.hpp"
#include "mnt/constraints/max_end_date_cost.hpp"
#include "mnt/constraints/min_start_date_cost.hpp"
#include "mnt/constraints/tag_conflict_cost.hpp"

namespace mnt
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider, every component at its own defaults (the quickstart's
// weights):
//
//   crewConflictCost             hard, weight 1
//   minStartDateCost             hard, weight 1
//   maxEndDateCost               hard, weight 1
//   beforeIdealEndDateCost       soft, weight 1
//   afterIdealEndDateCost        soft, weight 1'000'000
//   tagConflictCost              soft, weight 1'000
//
// This assembly scores bit-for-bit like ScoreCalculator::calculateScore and
// the monolithic incremental calculator (both read the same kDefaultWeight
// constants). It is also the domain's constraint catalog: constraint_info()
// reads the defaults off it, so they live in the component constructors and
// nowhere else. Its names() — and so list_constraints() — list the four
// per-job unit constraints first and the two global ones after them.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("afterIdealEndDateCost", 2)
//         .drop("beforeIdealEndDateCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<CrewConflictCost>();
    assembly.emplace<MinStartDateCost>();
    assembly.emplace<MaxEndDateCost>();
    assembly.emplace<BeforeIdealEndDateCost>();
    assembly.emplace<AfterIdealEndDateCost>();
    assembly.emplace<TagConflictCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_HPP

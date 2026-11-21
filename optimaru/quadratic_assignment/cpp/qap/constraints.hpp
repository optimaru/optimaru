#ifndef QAP_CONSTRAINTS_HPP
#define QAP_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (PinnedAssignmentsCost -> constraints/pinned_assignments_cost.hpp);
// common.hpp holds what all of them need (the calculator aliases, the
// "prepare() has run" guard, the member-change global base) and carries the
// pattern's documentation. Adding a component is a file plus a line here (and
// a line in defaultConstraints below when it belongs to the default assembly).

#include "qap/constraints/assignment_cost.hpp"
#include "qap/constraints/common.hpp"
#include "qap/constraints/flow_distance_cost.hpp"
#include "qap/constraints/forbidden_assignments_cost.hpp"
#include "qap/constraints/location_capacity_cost.hpp"
#include "qap/constraints/pinned_assignments_cost.hpp"

namespace qap {

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// Every component at its own defaults, in catalog (list_constraints()) order:
//
//   pinnedAssignmentsCost      hard, weight 1   (unit = location)
//   forbiddenAssignmentsCost   hard, weight 1   (unit = location)
//   locationCapacityCost       hard, weight 1   (unit = location)
//   assignmentCost             soft, weight 1   (global)
//   flowDistanceCost           soft, weight 1   (global)
//
// This assembly scores bit-for-bit like ScoreCalculator::score and
// the monolithic incremental calculator (all read the same kDefaultWeight
// constants). It is also the domain's constraint catalog: constraint_info()
// reads the defaults off it, so they live in the component constructors and
// nowhere else. It needs no problem to build.
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("flowDistanceCost", 4)
//         .soft("pinnedAssignmentsCost")
//         .drop("assignmentCost")
//         .into(calc);
inline ConstraintAssembly defaultConstraints() {
    ConstraintAssembly assembly;
    assembly.emplace<PinnedAssignmentsCost>();
    assembly.emplace<ForbiddenAssignmentsCost>();
    assembly.emplace<LocationCapacityCost>();
    assembly.emplace<AssignmentCost>();
    assembly.emplace<FlowDistanceCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator& calculator) {
    defaultConstraints().into(calculator);
}

}  // namespace qap

#endif  // QAP_CONSTRAINTS_HPP

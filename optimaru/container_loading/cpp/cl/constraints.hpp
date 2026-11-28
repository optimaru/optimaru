#ifndef CL_CONSTRAINTS_HPP
#define CL_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins -- umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (UnplacedItemCost -> constraints/unplaced_item_cost.hpp); common.hpp holds
// what all of them need (the calculator aliases, the shared decode) and
// carries the pattern's documentation.
//
// Adding a component is a file plus an include here and an `emplace` in
// defaultConstraints() below.

#include "cl/constraints/axle_load_cost.hpp"
#include "cl/constraints/balance_limit_cost.hpp"
#include "cl/constraints/common.hpp"
#include "cl/constraints/container_weight_cost.hpp"
#include "cl/constraints/group_split_cost.hpp"
#include "cl/constraints/incomplete_group_cost.hpp"
#include "cl/constraints/load_balance_cost.hpp"
#include "cl/constraints/optional_item_cost.hpp"
#include "cl/constraints/segregation_cost.hpp"
#include "cl/constraints/single_container_cost.hpp"
#include "cl/constraints/stacking_load_cost.hpp"
#include "cl/constraints/stacking_rule_cost.hpp"
#include "cl/constraints/unloading_order_cost.hpp"
#include "cl/constraints/unplaced_item_cost.hpp"
#include "cl/constraints/used_container_cost.hpp"
#include "cl/constraints/used_length_cost.hpp"

namespace cl {

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// Every component at its own defaults:
//
//   unplacedItemCost      hard, weight 1
//   containerWeightCost   hard, weight 1
//   stackingLoadCost      hard, weight 1
//   stackingRuleCost      hard, weight 1
//   unloadingOrderCost    hard, weight 1
//   balanceLimitCost      hard, weight 1
//   segregationCost       hard, weight 1
//   axleLoadCost          hard, weight 1
//   usedContainerCost     soft, weight 1
//   optionalItemCost      soft, weight 1
//   usedLengthCost        soft, weight 0 (opt-in)
//   loadBalanceCost       soft, weight 0 (opt-in)
//   groupSplitCost        soft, weight 1 (global)
//   incompleteGroupCost   hard, weight 1 (global)
//   singleContainerCost   hard, weight 1 (global)
//
// Units come before the globals in list_constraints() / constraint_info(), so
// the three globals are listed last. This assembly is what a solve without a
// selection scores with, and the domain's constraint catalog:
// constraint_info() reads the defaults off it, so they live in the component
// constructors and nowhere else. ScoreCalculator::score computes the same
// score as a full recompute.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("usedLengthCost", 0.001)
//         .drop("groupSplitCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints() {
    ConstraintAssembly assembly;
    assembly.emplace<UnplacedItemCost>();
    assembly.emplace<ContainerWeightCost>();
    assembly.emplace<StackingLoadCost>();
    assembly.emplace<StackingRuleCost>();
    assembly.emplace<UnloadingOrderCost>();
    assembly.emplace<BalanceLimitCost>();
    assembly.emplace<SegregationCost>();
    assembly.emplace<AxleLoadCost>();
    assembly.emplace<UsedContainerCost>();
    assembly.emplace<OptionalItemCost>();
    assembly.emplace<UsedLengthCost>();
    assembly.emplace<LoadBalanceCost>();
    assembly.emplace<GroupSplitCost>();
    assembly.emplace<IncompleteGroupCost>();
    assembly.emplace<SingleContainerCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator& calculator) {
    defaultConstraints().into(calculator);
}

}  // namespace cl

#endif  // CL_CONSTRAINTS_HPP

#ifndef KS_CONSTRAINTS_HPP
#define KS_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (KnapsackCapacityCost -> constraints/knapsack_capacity_cost.hpp);
// common.hpp holds what all of them need (the calculator aliases, the
// "prepare() has run" guard, the unit-1 SelectionConstraint base) and carries
// the pattern's documentation.
//
// Adding a component is a file plus a line here (and a line in
// defaultConstraints below when it belongs to the default assembly).

#include "ks/constraints/common.hpp"
#include "ks/constraints/forbidden_items_cost.hpp"
#include "ks/constraints/item_dependency_cost.hpp"
#include "ks/constraints/knapsack_capacity_cost.hpp"
#include "ks/constraints/mandatory_items_cost.hpp"
#include "ks/constraints/resource_capacity_cost.hpp"
#include "ks/constraints/selected_value_cost.hpp"
#include "ks/constraints/selection_group_bounds_cost.hpp"

namespace ks
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// Every constraint at its own defaults, in catalog order:
//
//   knapsackCapacityCost        hard, weight 1
//   resourceCapacityCost        hard, weight 1
//   mandatoryItemsCost          hard, weight 1
//   forbiddenItemsCost          hard, weight 1
//   selectionGroupBoundsCost    hard, weight 1
//   itemDependencyCost          hard, weight 1
//   selectedValueCost           soft, weight 1 (a reward: never hard)
//
// This assembly scores bit-for-bit like ScoreCalculator::score and
// the monolithic incremental calculator (both read the same kDefaultWeight
// constants). It is also the domain's constraint catalog: constraint_info()
// reads the defaults off it (all seven are globals, so info() keeps this
// order), so they live in the component constructors and nowhere else.
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("resourceCapacityCost", 4)
//         .soft("forbiddenItemsCost")
//         .drop("itemDependencyCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<KnapsackCapacityCost>();
    assembly.emplace<ResourceCapacityCost>();
    assembly.emplace<MandatoryItemsCost>();
    assembly.emplace<ForbiddenItemsCost>();
    assembly.emplace<SelectionGroupBoundsCost>();
    assembly.emplace<ItemDependencyCost>();
    assembly.emplace<SelectedValueCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace ks

#endif  // KS_CONSTRAINTS_HPP

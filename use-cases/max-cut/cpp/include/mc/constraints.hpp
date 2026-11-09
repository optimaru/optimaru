#ifndef MC_CONSTRAINTS_HPP
#define MC_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (FixedPartitionCost -> constraints/fixed_partition_cost.hpp); common.hpp
// holds what all of them need (the calculator aliases, the "prepare() has
// run" guard, the member-change base of the globals) and carries the
// pattern's documentation.
//
// Adding a component is a file plus a line here (and a line in
// defaultConstraints below when it belongs to the default assembly).

#include "mc/constraints/common.hpp"
#include "mc/constraints/cut_weight_cost.hpp"
#include "mc/constraints/different_partition_pairs_cost.hpp"
#include "mc/constraints/fixed_partition_cost.hpp"
#include "mc/constraints/partition_imbalance_cost.hpp"
#include "mc/constraints/partition_size_bounds_cost.hpp"
#include "mc/constraints/same_partition_pairs_cost.hpp"

namespace mc
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// Every constraint at its own defaults, in catalog order (the four unit
// constraints, then the two globals):
//
//   fixedPartitionCost           hard, weight 1
//   samePartitionPairsCost       hard, weight 1
//   differentPartitionPairsCost  hard, weight 1
//   partitionSizeBoundsCost      hard, weight 1
//   partitionImbalanceCost       soft, weight 0 (opt-in)
//   cutWeightCost                soft, weight 1 (a reward: never hard)
//
// This assembly scores bit-for-bit like ScoreCalculator::score and
// the monolithic incremental calculator (both read the same kDefaultWeight
// constants). It is also the domain's constraint catalog: constraint_info()
// reads the defaults off it, so they live in the component constructors and
// nowhere else.
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("partitionImbalanceCost", 1)
//         .soft("partitionSizeBoundsCost")
//         .drop("fixedPartitionCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<FixedPartitionCost>();
    assembly.emplace<SamePartitionPairsCost>();
    assembly.emplace<DifferentPartitionPairsCost>();
    assembly.emplace<PartitionSizeBoundsCost>();
    assembly.emplace<PartitionImbalanceCost>();
    assembly.emplace<CutWeightCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace mc

#endif  // MC_CONSTRAINTS_HPP

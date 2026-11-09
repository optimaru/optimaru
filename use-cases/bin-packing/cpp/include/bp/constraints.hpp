#ifndef BP_CONSTRAINTS_HPP
#define BP_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (BinCapacityCost -> constraints/bin_capacity_cost.hpp); common.hpp holds
// what all of them need (the calculator aliases and the "prepare() has run"
// guard) and carries the pattern's documentation.
//
// Adding a component is a file plus a line here (and a line in
// defaultConstraints below when it belongs to the default assembly).

#include "bp/constraints/bin_capacity_cost.hpp"
#include "bp/constraints/color_conflict_cost.hpp"
#include "bp/constraints/common.hpp"
#include "bp/constraints/item_conflict_cost.hpp"
#include "bp/constraints/used_bins_cost.hpp"

namespace bp
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// Every component at its own defaults:
//
//   binCapacityCost     hard, weight 1
//   colorConflictCost   hard, weight 1
//   itemConflictCost    hard, weight 1
//   usedBinsCost        soft, weight 1   (global: the bin count)
//
// This assembly scores bit-for-bit like ScoreCalculator::score and
// the monolithic incremental calculator (both read the same kDefaultWeight
// constants). It is also the domain's constraint catalog: constraint_info()
// reads the defaults off it, so they live in the component constructors and
// nowhere else. (Before 0.2.0 the problem carried the four default weights
// as capacityWeight / colorWeight / conflictWeight / usedBinWeight; 0.2.0
// removed them, so a weight comes only from the assembly.)
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly, so
// a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("usedBinsCost", 4)
//         .soft("binCapacityCost")
//         .drop("colorConflictCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<BinCapacityCost>();
    assembly.emplace<ColorConflictCost>();
    assembly.emplace<ItemConflictCost>();
    assembly.emplace<UsedBinsCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace bp

#endif  // BP_CONSTRAINTS_HPP

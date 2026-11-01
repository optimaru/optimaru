#ifndef OP_CONSTRAINTS_HPP
#define OP_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (TrolleyCapacityCost -> constraints/trolley_capacity_cost.hpp); common.hpp
// holds what all of them need (the calculator aliases) and carries the
// pattern's documentation.
//
// Adding a component is a file plus an include here (and an `emplace` in
// defaultConstraints() below when it belongs to the default assembly).

#include "op/constraints/common.hpp"
#include "op/constraints/distance_from_last_pick_task_to_path_origin_cost.hpp"
#include "op/constraints/distance_from_previous_pick_task_cost.hpp"
#include "op/constraints/order_split_by_trolley_cost.hpp"
#include "op/constraints/trolley_capacity_cost.hpp"

namespace op
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider, every component at its own defaults:
//
//   trolleyCapacityCost                        hard, weight 1
//   distanceFromPreviousPickTaskCost           soft, weight 1
//   distanceFromLastPickTaskToPathOriginCost   soft, weight 1
//   orderSplitByTrolleyCost                    soft, weight 1000
//
// This assembly scores bit-for-bit like ScoreCalculator::calculateScore and
// the monolithic incremental calculator (both read the same kDefaultWeight
// constants). It is also the domain's constraint catalog: constraint_info()
// reads the defaults off it, so they live in the component constructors and
// nowhere else.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("distanceFromLastPickTaskToPathOriginCost", 2)
//         .drop("distanceFromPreviousPickTaskCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<TrolleyCapacityCost>();
    assembly.emplace<DistanceFromPreviousPickTaskCost>();
    assembly.emplace<DistanceFromLastPickTaskToPathOriginCost>();
    assembly.emplace<OrderSplitByTrolleyCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace op

#endif  // OP_CONSTRAINTS_HPP

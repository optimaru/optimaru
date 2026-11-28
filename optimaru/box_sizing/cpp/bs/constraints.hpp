#ifndef BS_CONSTRAINTS_HPP
#define BS_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins: umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (ParcelFitCost -> constraints/parcel_fit_cost.hpp); common.hpp holds what
// all of them need (the calculator aliases, the two bases and the "prepare()
// has run" guard) and carries the pattern's documentation.
//
// Adding a component is a file plus a line here (and a line in
// defaultConstraints below when it belongs to the default assembly).

#include "bs/constraints/board_area_cost.hpp"
#include "bs/constraints/box_cost.hpp"
#include "bs/constraints/box_type_cost.hpp"
#include "bs/constraints/box_type_limit_cost.hpp"
#include "bs/constraints/box_weight_cost.hpp"
#include "bs/constraints/common.hpp"
#include "bs/constraints/parcel_fit_cost.hpp"
#include "bs/constraints/shipping_cost.hpp"
#include "bs/constraints/void_volume_cost.hpp"

namespace bs {

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// Every constraint at its own defaults, in catalog order (the unit
// constraints, then the globals):
//
//   parcelFitCost      hard, weight 1
//   boxWeightCost      hard, weight 1
//   voidVolumeCost     soft, weight 1
//   boxCost            soft, weight 1
//   boardAreaCost      soft, weight 0 (opt-in: the board of the boxes shipped)
//   shippingCost       soft, weight 0 (opt-in: shipping by billed weight)
//   boxTypeLimitCost   hard, weight 1
//   boxTypeCost        soft, weight 0 (opt-in: the fixed cost of one more size)
//
// This assembly scores like ScoreCalculator::score and the monolithic
// incremental calculator (they read the same kDefaultWeight constants). It is
// also the domain's constraint catalog: constraint_info() reads the defaults
// off it, so they live in the component constructors and nowhere else.
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("boxTypeCost", 50000)
//         .drop("boxCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints() {
    ConstraintAssembly assembly;
    assembly.emplace<ParcelFitCost>();
    assembly.emplace<BoxWeightCost>();
    assembly.emplace<VoidVolumeCost>();
    assembly.emplace<BoxCost>();
    assembly.emplace<BoardAreaCost>();
    assembly.emplace<ShippingCost>();
    assembly.emplace<BoxTypeLimitCost>();
    assembly.emplace<BoxTypeCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator& calculator) {
    defaultConstraints().into(calculator);
}

}  // namespace bs

#endif  // BS_CONSTRAINTS_HPP

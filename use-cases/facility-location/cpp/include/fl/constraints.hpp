#ifndef FL_CONSTRAINTS_HPP
#define FL_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (FacilityCapacityCost -> constraints/facility_capacity_cost.hpp); common.hpp
// holds what all of them need (the calculator aliases and the "prepare() has
// run" guard) and carries the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// defaultConstraints below when it belongs to the default assembly).

#include "fl/constraints/common.hpp"
#include "fl/constraints/distance_from_facility_cost.hpp"
#include "fl/constraints/facility_capacity_cost.hpp"
#include "fl/constraints/facility_setup_cost.hpp"

namespace fl
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider, every component at its own defaults:
//
//   facilityCapacityCost       hard, weight 1
//   facilitySetupCost          soft, weight 2
//   distanceFromFacilityCost   soft, weight 6
//
// This assembly scores bit-for-bit like ScoreCalculator::score and
// the monolithic incremental calculator (both read the same kDefaultWeight
// constants). It is also the domain's constraint catalog: constraint_info()
// reads the defaults off it, so they live in the component constructors and
// nowhere else.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly, so
// a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("facilitySetupCost", 4)
//         .soft("facilityCapacityCost")
//         .drop("distanceFromFacilityCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<FacilityCapacityCost>();
    assembly.emplace<FacilitySetupCost>();
    assembly.emplace<DistanceFromFacilityCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace fl

#endif  // FL_CONSTRAINTS_HPP

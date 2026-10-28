#ifndef FL_CONSTRAINTS_HPP
#define FL_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases and the "prepare() has run" guard) and carries
// the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include <cstdint>

#include "fl/constraints/common.hpp"
#include "fl/constraints/distance_from_facility.hpp"
#include "fl/constraints/facility_capacity.hpp"
#include "fl/constraints/facility_setup_cost.hpp"

namespace fl
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider: facilityCapacity hard at weight 1 plus the two soft terms
// at the caller's weights (defaults = the quickstart's effective ones). With
// the problem's own weights this assembly scores bit-for-bit like
// ScoreCalculator::calculateScore and the monolithic incremental calculator.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly, so
// a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints(problem)
//         .weight("facility setup cost", 4)
//         .soft("facility capacity")
//         .drop("distance from facility")
//         .into(calc);
//
// `addDefaultConstraints(calc, ...)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly
defaultConstraints(std::int64_t setupCostWeight = 2,
                   std::int64_t distanceFromFacilityWeight = 6)
{
    ConstraintAssembly assembly;
    assembly.emplace<FacilityCapacity>();
    assembly.emplace<FacilitySetupCost>(static_cast<double>(setupCostWeight));
    assembly.emplace<DistanceFromFacility>(
        static_cast<double>(distanceFromFacilityWeight));
    return assembly;
}

// Same assembly, with the weights taken off the solution — where the
// quickstart's ConstraintWeightOverrides land (see domain.hpp).
inline ConstraintAssembly
defaultConstraints(const FacilityLocationProblem &problem)
{
    return defaultConstraints(problem.setupCostWeight,
                              problem.distanceFromFacilityWeight);
}

inline void addDefaultConstraints(DecomposedCalculator &calculator,
                                  std::int64_t setupCostWeight = 2,
                                  std::int64_t distanceFromFacilityWeight = 6)
{
    defaultConstraints(setupCostWeight, distanceFromFacilityWeight)
        .into(calculator);
}

inline void addDefaultConstraints(DecomposedCalculator &calculator,
                                  const FacilityLocationProblem &problem)
{
    defaultConstraints(problem).into(calculator);
}

}  // namespace fl

#endif  // FL_CONSTRAINTS_HPP

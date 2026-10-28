#ifndef VRP_CONSTRAINTS_HPP
#define VRP_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins -- umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases and the route chain walk) and carries the
// pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "vrp/constraints/common.hpp"
#include "vrp/constraints/load_utilization.hpp"
#include "vrp/constraints/maximize_visits_assigned.hpp"
#include "vrp/constraints/minimize_travel_time.hpp"
#include "vrp/constraints/route_costs.hpp"
#include "vrp/constraints/route_geometry.hpp"
#include "vrp/constraints/route_limits.hpp"
#include "vrp/constraints/route_sums.hpp"
#include "vrp/constraints/service_finished_after_max_end_time.hpp"
#include "vrp/constraints/vehicle_capacity.hpp"

namespace vrp {

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider at the quickstart's level weights. This assembly scores
// bit-for-bit like ScoreCalculator::calculateScore.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("maximizeVisitsAssigned", 2)
//         .drop("minimizeTravelTime")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints() {
    ConstraintAssembly assembly;
    assembly.emplace<VehicleCapacity>();
    assembly.emplace<ServiceFinishedAfterMaxEndTime>();
    assembly.emplace<MaximizeVisitsAssigned>();
    assembly.emplace<MinimizeTravelTime>();
    assembly.emplace<MaxTravelTime>();
    assembly.emplace<MaxRouteDuration>();
    assembly.emplace<VehicleEndTime>();
    assembly.emplace<MaxStops>();
    assembly.emplace<MaxRouteRadius>();
    assembly.emplace<MinimizeEnclosingCircleRadius>();
    assembly.emplace<MinCapacityUtilization>();
    assembly.emplace<MinimizeRouteDuration>();
    assembly.emplace<MinimizeLatency>();
    assembly.emplace<VehicleFixedCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator& calculator) {
    defaultConstraints().into(calculator);
}

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_HPP

#ifndef PDP_CONSTRAINTS_HPP
#define PDP_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases, the route chain walk and the request-coupling
// global base) and carries the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "pdp/constraints/common.hpp"
#include "pdp/constraints/load_utilization.hpp"
#include "pdp/constraints/minimize_travel_time.hpp"
#include "pdp/constraints/paired_request.hpp"
#include "pdp/constraints/pickup_before_dropoff.hpp"
#include "pdp/constraints/route_costs.hpp"
#include "pdp/constraints/route_geometry.hpp"
#include "pdp/constraints/route_limits.hpp"
#include "pdp/constraints/same_vehicle.hpp"
#include "pdp/constraints/service_finished_after_max_end_time.hpp"
#include "pdp/constraints/unassigned_requests.hpp"
#include "pdp/constraints/vehicle_capacity.hpp"

namespace pdp
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full constraint set at its default weights. This assembly scores
// bit-for-bit like ScoreCalculator::calculateScore.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("minimizeTravelTime", 2)
//         .drop("pairedRequest")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<PairedRequest>();
    assembly.emplace<SameVehicle>();
    assembly.emplace<PickupBeforeDropoff>();
    assembly.emplace<VehicleCapacity>();
    assembly.emplace<ServiceFinishedAfterMaxEndTime>();
    assembly.emplace<UnassignedRequests>();
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

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_HPP

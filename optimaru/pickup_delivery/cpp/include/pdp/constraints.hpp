#ifndef PDP_CONSTRAINTS_HPP
#define PDP_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (VehicleCapacityCost -> constraints/vehicle_capacity_cost.hpp); common.hpp
// holds what all of them need (the calculator aliases, the route chain walk
// and the request-coupling global base) and carries the pattern's
// documentation, and route_geometry.hpp the enclosing-circle helpers.
//
// Adding a component is a file plus an include here (and an `emplace` in
// defaultConstraints() below when it belongs to the default assembly).

#include "pdp/constraints/common.hpp"
#include "pdp/constraints/enclosing_circle_radius_cost.hpp"
#include "pdp/constraints/latency_cost.hpp"
#include "pdp/constraints/max_end_time_cost.hpp"
#include "pdp/constraints/max_route_duration_cost.hpp"
#include "pdp/constraints/max_route_radius_cost.hpp"
#include "pdp/constraints/max_stops_cost.hpp"
#include "pdp/constraints/max_travel_time_cost.hpp"
#include "pdp/constraints/min_capacity_utilization_cost.hpp"
#include "pdp/constraints/paired_request_cost.hpp"
#include "pdp/constraints/pickup_before_dropoff_cost.hpp"
#include "pdp/constraints/pinned_stops_cost.hpp"
#include "pdp/constraints/required_vehicle_cost.hpp"
#include "pdp/constraints/route_duration_cost.hpp"
#include "pdp/constraints/same_vehicle_cost.hpp"
#include "pdp/constraints/squared_order_age_cost.hpp"
#include "pdp/constraints/travel_time_cost.hpp"
#include "pdp/constraints/unassigned_requests_cost.hpp"
#include "pdp/constraints/vehicle_capacity_cost.hpp"
#include "pdp/constraints/vehicle_fixed_cost.hpp"
#include "pdp/constraints/vehicle_max_end_time_cost.hpp"

namespace pdp {

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full constraint set, every component at its own defaults
// (kDefaultWeight / kDefaultLevel in its header):
//
//   pairedRequestCost            hard, weight 1000
//   sameVehicleCost              hard, weight 1000
//   pickupBeforeDropoffCost      hard, weight 1000
//   vehicleCapacityCost          hard, weight 1
//   maxEndTimeCost               hard, weight 1
//   unassignedRequestsCost       medium, weight 1 (folded into soft x 10^9)
//   travelTimeCost               soft, weight 1
//   maxTravelTimeCost            hard, weight 1   (0 while the vehicle
//   maxRouteDurationCost         hard, weight 1    leaves the limit at 0)
//   vehicleMaxEndTimeCost        hard, weight 1
//   maxStopsCost                 hard, weight 1
//   maxRouteRadiusCost           hard, weight 1
//   enclosingCircleRadiusCost    soft, weight 0   (opt-in)
//   minCapacityUtilizationCost   soft, weight 1
//   routeDurationCost            soft, weight 0   (opt-in)
//   latencyCost                  soft, weight 0   (opt-in)
//   squaredOrderAgeCost          soft, weight 0   (opt-in)
//   vehicleFixedCost             soft, weight 1
//   pinnedStopsCost              hard, weight 1e12
//   requiredVehicleCost          hard, weight 1000
//
// On a plan that sets no route policy, pins nothing and binds no request,
// this assembly scores bit-for-bit like ScoreCalculator::score (both
// read the same kDefaultWeight constants). It is also the domain's constraint
// catalog: constraint_info() reads the defaults off it, so they live in the
// component constructors and nowhere else.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("travelTimeCost", 2)
//         .drop("pairedRequestCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints() {
    ConstraintAssembly assembly;
    assembly.emplace<PairedRequestCost>();
    assembly.emplace<SameVehicleCost>();
    assembly.emplace<PickupBeforeDropoffCost>();
    assembly.emplace<VehicleCapacityCost>();
    assembly.emplace<MaxEndTimeCost>();
    assembly.emplace<UnassignedRequestsCost>();
    assembly.emplace<TravelTimeCost>();
    assembly.emplace<MaxTravelTimeCost>();
    assembly.emplace<MaxRouteDurationCost>();
    assembly.emplace<VehicleMaxEndTimeCost>();
    assembly.emplace<MaxStopsCost>();
    assembly.emplace<MaxRouteRadiusCost>();
    assembly.emplace<EnclosingCircleRadiusCost>();
    assembly.emplace<MinCapacityUtilizationCost>();
    assembly.emplace<RouteDurationCost>();
    assembly.emplace<LatencyCost>();
    assembly.emplace<SquaredOrderAgeCost>();
    assembly.emplace<VehicleFixedCost>();
    assembly.emplace<PinnedStopsCost>();
    assembly.emplace<RequiredVehicleCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator& calculator) {
    defaultConstraints().into(calculator);
}

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_HPP

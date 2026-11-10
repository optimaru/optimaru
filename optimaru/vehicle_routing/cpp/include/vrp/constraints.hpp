#ifndef VRP_CONSTRAINTS_HPP
#define VRP_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins -- umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (<subject>_cost.hpp); common.hpp holds what all of them need (the calculator
// aliases and the route chain walk) and carries the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "vrp/constraints/common.hpp"
#include "vrp/constraints/enclosing_circle_radius_cost.hpp"
#include "vrp/constraints/latency_cost.hpp"
#include "vrp/constraints/max_end_time_cost.hpp"
#include "vrp/constraints/max_route_duration_cost.hpp"
#include "vrp/constraints/max_route_radius_cost.hpp"
#include "vrp/constraints/max_stops_cost.hpp"
#include "vrp/constraints/max_travel_time_cost.hpp"
#include "vrp/constraints/min_capacity_utilization_cost.hpp"
#include "vrp/constraints/route_duration_cost.hpp"
#include "vrp/constraints/route_geometry.hpp"
#include "vrp/constraints/route_sums.hpp"
#include "vrp/constraints/travel_time_cost.hpp"
#include "vrp/constraints/unassigned_visits_cost.hpp"
#include "vrp/constraints/vehicle_capacity_cost.hpp"
#include "vrp/constraints/vehicle_fixed_cost.hpp"
#include "vrp/constraints/vehicle_max_end_time_cost.hpp"

namespace vrp {

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider plus the route policies, each at its component's
// kDefaultWeight / kDefaultLevel. This assembly scores bit-for-bit like
// ScoreCalculator::score on a plan without route policies (the
// policy components cost 0 there, the opt-in objectives weigh 0). Units come
// before the global in list_constraints() / constraint_info(), so
// unassignedVisitsCost is listed last.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("unassignedVisitsCost", 2)
//         .drop("travelTimeCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints() {
    ConstraintAssembly assembly;
    assembly.emplace<VehicleCapacityCost>();
    assembly.emplace<MaxEndTimeCost>();
    assembly.emplace<UnassignedVisitsCost>();
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
    assembly.emplace<VehicleFixedCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator& calculator) {
    defaultConstraints().into(calculator);
}

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_HPP

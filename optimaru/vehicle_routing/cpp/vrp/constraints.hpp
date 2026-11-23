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

#include <stdexcept>

#include "vrp/constraints/common.hpp"
#include "vrp/constraints/enclosing_circle_radius_cost.hpp"
#include "vrp/constraints/latency_cost.hpp"
#include "vrp/constraints/max_end_time_cost.hpp"
#include "vrp/constraints/max_route_duration_cost.hpp"
#include "vrp/constraints/max_route_radius_cost.hpp"
#include "vrp/constraints/max_stops_cost.hpp"
#include "vrp/constraints/max_travel_time_cost.hpp"
#include "vrp/constraints/min_capacity_utilization_cost.hpp"
#include "vrp/constraints/profile_forbidden_visit_cost.hpp"
#include "vrp/constraints/profile_preferred_visit_cost.hpp"
#include "vrp/constraints/profile_visit_table.hpp"
#include "vrp/constraints/route_duration_cost.hpp"
#include "vrp/constraints/route_geometry.hpp"
#include "vrp/constraints/route_sums.hpp"
#include "vrp/constraints/travel_distance_cost.hpp"
#include "vrp/constraints/travel_time_cost.hpp"
#include "vrp/constraints/unassigned_visits_cost.hpp"
#include "vrp/constraints/vehicle_capacity_cost.hpp"
#include "vrp/constraints/vehicle_fixed_cost.hpp"
#include "vrp/constraints/vehicle_max_end_time_cost.hpp"
#include "vrp/constraints/visit_group_cost.hpp"
#include "vrp/constraints/visit_order_cost.hpp"

namespace vrp {

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider plus the route policies, each at its component's
// kDefaultWeight / kDefaultLevel. This assembly scores bit-for-bit like
// ScoreCalculator::score on a plan without route policies, without
// visit groups and without profile visit lists (the policy, group and profile
// components cost 0 there, the opt-in objectives weigh 0). Units come before the globals in
// list_constraints() / constraint_info(), so visitGroupCost and unassignedVisitsCost are listed
// last.
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
    assembly.emplace<VisitGroupCost>();
    assembly.emplace<UnassignedVisitsCost>();
    assembly.emplace<TravelTimeCost>();
    assembly.emplace<TravelDistanceCost>();
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
    assembly.emplace<VisitOrderCost>();
    assembly.emplace<ProfileForbiddenVisitCost>();
    assembly.emplace<ProfilePreferredVisitCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator& calculator) {
    defaultConstraints().into(calculator);
}

// A caller's selection as every solve scores it. profileForbiddenVisitCost
// is always on: a selection that leaves it out gets it at its defaults,
// since a forbidden visit is no preference to weigh but a job the vehicle
// cannot do (on a plan without forbidden visits it costs 0, so nothing
// changes there). A plan that prefers visits needs profilePreferredVisitCost
// in the selection, the only cost that reads those lists: without it they
// would be silently ignored, so that throws std::invalid_argument.
inline ConstraintAssembly withAlwaysOnConstraints(ConstraintAssembly assembly,
                                                  const VehicleRoutePlan& plan) {
    if (!assembly.contains("profileForbiddenVisitCost") && plan.hasForbiddenVisits())
        assembly.emplace<ProfileForbiddenVisitCost>();
    if (!assembly.contains("profilePreferredVisitCost") && plan.hasPreferredVisits())
        throw std::invalid_argument(
                "a vehicle's profile has preferred visits, but the constraints leave out "
                "profile_preferred_visit_cost, the cost that scores them: add it, or leave the "
                "preferred visits out");
    return assembly;
}

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_HPP

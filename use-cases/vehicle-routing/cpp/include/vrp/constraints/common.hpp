#ifndef VRP_CONSTRAINTS_COMMON_HPP
#define VRP_CONSTRAINTS_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <vector>

#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "vrp/domain.hpp"
#include "vrp/score_calculator.hpp"

namespace vrp {

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The four constraints of the Timefold vehicle-routing
// VehicleRoutingConstraintProvider, split into one small component each on the
// engine's DecomposedScoreCalculator -- an optional assembly path NEXT TO
// ScoreCalculator::calculateScore, which stays the default path.
//
// Unit choice: with the LIST descriptor the calculator's unit is one LIST,
// i.e. one vehicle; members are its visits IN ROUTE ORDER. Three rules are
// per-route sums and run as UnitConstraints. Only maximizeVisitsAssigned
// counts the visits in NO route at all, which belongs to no unit, so it runs
// as a GlobalConstraint.
//
// A component never reads the Visit shadow variables (vehicleIndex,
// arrivalTime): walkRoute below re-derives the arrival chain from the unit's
// own member order, which is what makes each cost a pure function of
// (solution, unit, members) as the engine's contract requires. The LIST path
// re-costs a touched route whole, so no delta contract is needed. The four
// default components also answer the engine's move previews in O(pieces)
// from shared per-route prefix sums (route_sums.hpp); the optional policies
// are previewed by writing the candidate route out and calling cost().
//
// Score levels: the quickstart scores with HardMediumSoftScore, which this
// domain folds into the engine's HardSoftScore as
// `soft = medium * kMediumUnit + soft` (see domain.hpp). The medium-level
// component therefore carries the kMediumUnit factor in its default weight.
//
//   DecomposedCalculator calc;
//   defaultConstraints()              // the whole default set
//       .weight("...", 2)            // ... then only what differs
//       .soft("...")
//       .drop("...")
//       .into(calc);
//   solver.solve(plan, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// The level and the weight of each component are DEFAULTS -- assembly-time
// metadata, not part of the cost. Override them per instance via the mutable
// `isHard` / `weight` / `exponent`.
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty; the calculator negates into the score. An assembly of all four
// components at their default weights therefore scores bit-for-bit like
// ScoreCalculator::calculateScore.

using DecomposedCalculator = optimaru::DecomposedScoreCalculator<VehicleRoutePlan>;
using ConstraintAssembly = optimaru::ConstraintAssembly<VehicleRoutePlan>;
using VrpUnitConstraint = optimaru::UnitConstraint<VehicleRoutePlan>;
using VrpGlobalConstraint = optimaru::GlobalConstraint<VehicleRoutePlan>;

namespace detail {

struct RouteMetrics {
    Seconds driving = 0;
    Seconds endTime = 0;
    Seconds latency = 0;
};

// Is this list the virtual unassigned pool rather than a real vehicle? The
// descriptor exposes the pool as one extra LIST after the vehicles, so every
// per-route component has to step over it.
inline bool isPool(const VehicleRoutePlan& plan, int unit) {
    return unit >= static_cast<int>(plan.vehicles.size());
}

// The Java arrivalTimeSupplier / getDepartureTime chain of one route,
// re-derived from its member order:
//   arrival      = previous departure + drivingTime(previous -> this)
//   startService = max(arrival, minStartTime)
//   departure    = startService + serviceDuration
// Calls onVisit(visitIndex, arrival) in order and returns the driving time of
// the whole route, home -> visits... and back home only when the vehicle
// returns to its depot (0 for an empty route).
template <typename F>
inline Seconds walkRoute(const VehicleRoutePlan& plan, int vehicleIndex,
                         const std::vector<int>& members, F&& onVisit) {
    if (members.empty()) return 0;
    const Vehicle& vehicle = plan.vehicles[vehicleIndex];
    Seconds previousDeparture = vehicle.departureTime;
    int previousLocation = vehicle.homeLocationIndex;
    Seconds driving = 0;
    for (int visitIndex : members) {
        const Visit& visit = plan.visits[visitIndex];
        const Seconds leg = plan.drivingTime(previousLocation, visit.locationIndex);
        const Seconds arrival = previousDeparture + leg;
        driving += leg;
        onVisit(visitIndex, arrival);
        previousDeparture = std::max(arrival, visit.minStartTime) + visit.serviceDuration;
        previousLocation = visit.locationIndex;
    }
    driving += vehicle.homeLeg(plan.drivingSeconds, previousLocation);
    return driving;
}

inline RouteMetrics routeMetrics(const VehicleRoutePlan& plan, int vehicleIndex,
                                 const std::vector<int>& members) {
    const Vehicle& vehicle = plan.vehicles[vehicleIndex];
    RouteMetrics metrics{0, vehicle.departureTime, 0};
    if (members.empty()) return metrics;
    Seconds previousDeparture = vehicle.departureTime;
    int previousLocation = vehicle.homeLocationIndex;
    for (int visitIndex : members) {
        const Visit& visit = plan.visits[visitIndex];
        const Seconds leg = plan.drivingTime(previousLocation, visit.locationIndex);
        const Seconds arrival = previousDeparture + leg;
        metrics.driving += leg;
        metrics.latency += std::max<Seconds>(0, arrival - vehicle.departureTime);
        previousDeparture = std::max(arrival, visit.minStartTime) + visit.serviceDuration;
        previousLocation = visit.locationIndex;
    }
    // An open route ends at its last visit's departure; a closed one after
    // the drive back home.
    const Seconds homeLeg = vehicle.homeLeg(plan.drivingSeconds, previousLocation);
    metrics.driving += homeLeg;
    metrics.endTime = previousDeparture + homeLeg;
    return metrics;
}

inline double haversineMeters(const Location& a, const Location& b) {
    constexpr double earthRadius = 6371000.0;
    constexpr double pi = 3.14159265358979323846;
    const double lat1 = a.latitude * pi / 180.0;
    const double lat2 = b.latitude * pi / 180.0;
    const double dlat = lat2 - lat1;
    const double dlon = (b.longitude - a.longitude) * pi / 180.0;
    const double h = std::sin(dlat / 2.0) * std::sin(dlat / 2.0)
                     + std::cos(lat1) * std::cos(lat2)
                           * std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
    return 2.0 * earthRadius * std::asin(std::sqrt(std::min(1.0, h)));
}

}  // namespace detail

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_COMMON_HPP

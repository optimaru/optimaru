#ifndef VRP_CONSTRAINTS_COMMON_HPP
#define VRP_CONSTRAINTS_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "vrp/domain.hpp"
#include "vrp/score_calculator.hpp"

namespace vrp {

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The constraints of the reference vehicle-routing
// VehicleRoutingConstraintProvider plus the optional route policies, one small
// component each on the engine's DecomposedScoreCalculator -- an optional
// assembly path NEXT TO ScoreCalculator::calculateScore, which stays the
// default path for the four core rules.
//
// Unit choice: with the LIST descriptor the calculator's unit is one LIST,
// i.e. one vehicle; members are its visits IN ROUTE ORDER. Every rule but one
// is a per-route sum and runs as a UnitConstraint over the vehicles
// (detail::PerVehicleConstraint: the pool list is no vehicle, so it is not
// one of their units). Only unassignedVisitsCost counts the visits in NO
// route at all, which belongs to no unit, so it runs as a GlobalConstraint.
//
// A component never reads the Visit shadow variables (vehicleIndex,
// arrivalTime): walkRoute below re-derives the arrival chain from the unit's
// own member order, which is what makes each cost a pure function of
// (solution, unit, members) as the engine's contract requires. The LIST path
// re-costs a touched route whole, so no delta contract is needed. The four
// core components also answer the engine's move previews in O(pieces) from
// shared per-route prefix sums (route_sums.hpp); the optional policies are
// previewed by writing the candidate route out and calling cost().
//
// Naming: one header per component, named after its struct
// (MaxTravelTimeCost -> max_travel_time_cost.hpp). The registry name is the
// struct name with a lower-case first letter ("maxTravelTimeCost"); Python
// sees it through pythonName() ("max_travel_time_cost"). Helpers that are not
// components live in route_sums.hpp (the shared prefix sums) and
// route_geometry.hpp (the enclosing circle).
//
// Defaults: each component declares them once, as static constexpr
// kDefaultWeight / kDefaultLevel passed to the base constructor. They are
// assembly-time metadata, not part of the cost. Override them per instance
// with the constructor's weight argument or the mutable `level` / `weight` /
// `exponent` / `offset` / `clip` (validated setters setWeight / setLevel /
// setExponent / setOffset / setClip), or by name through ConstraintAssembly.
// Read the level with `isHard()` or `level`. A name may be held several times
// (ConstraintAssembly::addInstances): each instance has its own knobs and the
// instances add up.
//
// Score levels: the quickstart scores with HardMediumSoftScore. The medium
// level (unassignedVisitsCost) is folded into the engine's HardSoftScore by
// the calculator as `soft = medium * kMediumUnit + soft` (kMediumFold in
// domain.hpp), so a medium weight is in medium units: weight 1 = one medium
// unit per penalty minute. The opt-in objectives (enclosingCircleRadiusCost,
// routeDurationCost, latencyCost) default to weight 0.
//
//   DecomposedCalculator calc;
//   defaultConstraints()                     // the whole default set
//       .weight("travelTimeCost", 2)         // ... then only what differs
//       .soft("vehicleCapacityCost")
//       .drop("maxStopsCost")
//       .into(calc);
//   solver.solve(plan, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty, 0 = no violation; the calculator negates into the score. The
// default assembly therefore scores bit-for-bit like
// ScoreCalculator::calculateScore on a plan without route policies.

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

// The base of every per-vehicle component: a unit constraint that EVALUATES
// the vehicles and not the pool list (scopedUnits() = 0 .. vehicles - 1, set
// at reset). The pool's cost is 0 either way, so no score changes; what the
// scope decides is where the literal offset formula (clip = false) charges
// shapeCost(0 - offset): every vehicle, an idle one included, and never the
// pool, which is no vehicle. constraint_costs() reports the scope as
// "units", so the Python score() charges the same vehicles. A component
// that needs its own onReset() calls this one first.
struct PerVehicleConstraint : VrpUnitConstraint {
    using VrpUnitConstraint::VrpUnitConstraint;

    void onReset(const VehicleRoutePlan& plan) override {
        vehicles_.resize(plan.vehicles.size());
        std::iota(vehicles_.begin(), vehicles_.end(), 0);
    }
    const std::vector<int>* scopedUnits() const override { return &vehicles_; }

private:
    std::vector<int> vehicles_;
};

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

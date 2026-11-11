#ifndef PDP_CONSTRAINTS_COMMON_HPP
#define PDP_CONSTRAINTS_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

#include "pdp/domain.hpp"
#include "pdp/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace pdp {

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The PDPTW constraints, split into one small component each on the engine's
// DecomposedObjective — an optional assembly path NEXT TO
// ScoreCalculator::score and the monolithic incremental calculator,
// which stay the tuned default path for the seven core constraints.
//
// Unit choice: with the sequence shape the calculator's unit is one list,
// i.e. one vehicle; members are its nodes IN ROUTE ORDER. The solution
// traits expose the unassigned pool as one extra list after the vehicles, so every
// per-route component steps over it (detail::isPool) and evaluates the
// vehicles only (detail::PerVehicleConstraint); requiredVehicleCost and
// squaredOrderAgeCost price the pool too and evaluate every list.
//
// The per-route rules run as UnitConstraints. The three coupling constraints relate
// a request's pickup to its dropoff — which may sit in different routes, which
// is the whole point of the rule — and unassignedRequestsCost counts requests
// in no route at all, so those four are GlobalConstraints
// (RequestCouplingConstraint below).
//
// A component never reads the Node derived fields (vehicleIndex,
// arrivalTime): walkRoute below re-derives the arrival chain from the unit's
// own member order, which is what makes each cost a pure function of
// (solution, unit, members) as the engine's contract requires. The sequence path
// re-costs a touched route whole, so no delta contract is needed.
//
// Naming: one header per component, named after its struct
// (MaxTravelTimeCost -> max_travel_time_cost.hpp). The registry name is the
// struct name with a lower-case first letter ("maxTravelTimeCost"); Python
// sees it through python_name() ("max_travel_time_cost").
//
// Defaults: each component declares them once, as static constexpr
// kDefaultWeight / kDefaultLevel passed to the base constructor. They are
// assembly-time metadata, not part of the cost. Override them per instance
// with the constructor's weight argument or the mutable `level` / `weight` /
// `exponent` / `offset` / `clip` (validated setters set_weight / set_level /
// set_exponent / set_offset / set_clip), or by name through ConstraintAssembly.
// Read the level with `is_hard()` or `level`. A name may be held several times
// (ConstraintAssembly::add_instances): each instance has its own knobs and the
// instances add up.
//
// Score levels: hard, medium, soft. The medium level (unassignedRequestsCost) is
// folded into the engine's Score by the calculator as
// `soft = medium * kMediumUnit + soft` (kMediumFold in domain.hpp), so a
// medium weight is in medium units: weight 1 = one medium unit per minute.
// The three coupling constraints and requiredVehicleCost default to
// ScoreCalculator::kCouplingPenalty, which is far above a typical lateness so
// the search repairs broken pairs before polishing time windows;
// pinnedStopsCost defaults to ScoreCalculator::kPinnedPenalty. The opt-in
// objectives (enclosingCircleRadiusCost, routeDurationCost, latencyCost,
// squaredOrderAgeCost) default to weight 0.
//
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md. The default assembly scores bit-for-bit
// like ScoreCalculator::score on a plan without route policies.

using DecomposedCalculator = optimaru::DecomposedObjective<PdpRoutePlan>;
using ConstraintAssembly = optimaru::ConstraintAssembly<PdpRoutePlan>;
using PdpUnitConstraint = optimaru::UnitConstraint<PdpRoutePlan>;
using PdpGlobalConstraint = optimaru::GlobalConstraint<PdpRoutePlan>;

namespace detail {

struct RouteMetrics {
    Seconds driving = 0;
    Seconds endTime = 0;
    Seconds latency = 0;
};

// Is this list the virtual unassigned pool rather than a real vehicle?
inline bool isPool(const PdpRoutePlan& plan, int unit) {
    return unit >= static_cast<int>(plan.vehicles.size());
}

// The base of every per-vehicle component: a unit constraint that EVALUATES
// the vehicles and not the pool list (scoped_units() = 0 .. vehicles - 1, set
// at reset). The pool's cost is 0 either way, so no score changes; what the
// scope decides is where the literal offset formula (clip = false) charges
// shape_cost(0 - offset): every vehicle, an idle one included, and never the
// pool, which is no vehicle. constraint_costs() reports the scope as
// "units", so the Python score() charges the same vehicles. The two
// components that DO price the pool (requiredVehicleCost,
// squaredOrderAgeCost) stay on PdpUnitConstraint and evaluate every list.
struct PerVehicleConstraint : PdpUnitConstraint {
    using PdpUnitConstraint::PdpUnitConstraint;

    void on_reset(const PdpRoutePlan& plan) override {
        vehicles_.resize(plan.vehicles.size());
        std::iota(vehicles_.begin(), vehicles_.end(), 0);
    }
    const std::vector<int>* scoped_units() const override { return &vehicles_; }

private:
    std::vector<int> vehicles_;
};

// The arrival chain of one route, re-derived from its member order:
//   arrival      = previous departure + drivingTime(previous -> this)
//   departure    = max(arrival, minStartTime) + serviceDuration
// Calls onNode(nodeIndex, arrival) in order and returns the driving time of
// the whole route, home -> nodes... and back home only when the vehicle
// returns to its depot (0 for an empty route).
template <typename F>
inline Seconds walkRoute(const PdpRoutePlan& plan, int vehicleIndex,
                         const std::vector<int>& members, F&& onNode) {
    if (members.empty()) return 0;
    const Vehicle& vehicle = plan.vehicles[vehicleIndex];
    Seconds previousDeparture = vehicle.departureTime;
    int previousLocation = vehicle.homeLocationIndex;
    Seconds driving = 0;
    for (int nodeIndex : members) {
        const Node& node = plan.nodes[static_cast<std::size_t>(nodeIndex)];
        const Seconds leg = plan.drivingTime(previousLocation, node.locationIndex);
        const Seconds arrival = previousDeparture + leg;
        driving += leg;
        onNode(nodeIndex, arrival);
        previousDeparture = std::max(arrival, node.minStartTime) + node.serviceDuration;
        previousLocation = node.locationIndex;
    }
    driving += vehicle.homeLeg(plan.drivingSeconds, previousLocation);
    return driving;
}

inline RouteMetrics routeMetrics(const PdpRoutePlan& plan, int vehicleIndex,
                                 const std::vector<int>& members) {
    const Vehicle& vehicle = plan.vehicles[vehicleIndex];
    RouteMetrics metrics{0, vehicle.departureTime, 0};
    if (members.empty()) return metrics;
    Seconds previousDeparture = vehicle.departureTime;
    int previousLocation = vehicle.homeLocationIndex;
    for (int nodeIndex : members) {
        const Node& node = plan.nodes[static_cast<std::size_t>(nodeIndex)];
        const Seconds leg = plan.drivingTime(previousLocation, node.locationIndex);
        const Seconds arrival = previousDeparture + leg;
        metrics.driving += leg;
        metrics.latency += std::max<Seconds>(0, arrival - vehicle.departureTime);
        previousDeparture = std::max(arrival, node.minStartTime) + node.serviceDuration;
        previousLocation = node.locationIndex;
    }
    // An open route ends at its last stop's departure; a closed one after
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
    const double h = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
                     std::cos(lat1) * std::cos(lat2) * std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
    return 2.0 * earthRadius * std::asin(std::sqrt(std::min(1.0, h)));
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Request-coupling global constraints
// ---------------------------------------------------------------------------
// Shared base of the three rules that judge a request's pickup against its
// dropoff. It tracks where each node currently sits and at which position —
// the solution's own derived fields cannot be used, because on a leave
// notification they are already stale — and re-scores whole routes on every
// change, which keeps positions exact without a delta argument.
struct RequestCouplingConstraint : PdpGlobalConstraint {
    using PdpGlobalConstraint::PdpGlobalConstraint;

    // The penalty units of one request given where its two nodes sit
    // (unit == -1 means "in no route").
    virtual std::int64_t requestCost(const PdpRoutePlan& plan, const Delivery& delivery,
                                     int pickupUnit, int pickupPosition, int dropoffUnit,
                                     int dropoffPosition) const = 0;

    void reset(const PdpRoutePlan& plan,
               const std::vector<std::vector<int>>& members_per_unit) override {
        plan_ = &plan;
        unitOf_.assign(plan.nodes.size(), -1);
        positionOf_.assign(plan.nodes.size(), -1);
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit) {
            if (detail::isPool(plan, static_cast<int>(unit))) continue;
            record(static_cast<int>(unit), members_per_unit[unit]);
        }
        recompute();
    }

    void on_unit_changed(const PdpRoutePlan& plan, int unit,
                         const std::vector<int>& members) override {
        plan_ = &plan;
        // Clear whatever used to sit in this unit, then record it anew.
        for (std::size_t node = 0; node < unitOf_.size(); ++node) {
            if (unitOf_[node] == unit) {
                unitOf_[node] = -1;
                positionOf_[node] = -1;
            }
        }
        if (!detail::isPool(plan, unit)) record(unit, members);
        recompute();
    }

    double cost() const override { return static_cast<double>(total_); }

private:
    void record(int unit, const std::vector<int>& members) {
        for (std::size_t p = 0; p < members.size(); ++p) {
            const std::size_t node = static_cast<std::size_t>(members[p]);
            unitOf_[node] = unit;
            positionOf_[node] = static_cast<int>(p);
        }
    }

    void recompute() {
        total_ = 0;
        for (const Delivery& delivery : plan_->deliveries) {
            const std::size_t pickup = static_cast<std::size_t>(delivery.pickupNodeIndex);
            const std::size_t dropoff = static_cast<std::size_t>(delivery.dropoffNodeIndex);
            total_ += requestCost(*plan_, delivery, unitOf_[pickup], positionOf_[pickup],
                                  unitOf_[dropoff], positionOf_[dropoff]);
        }
    }

    const PdpRoutePlan* plan_ = nullptr;
    std::vector<int> unitOf_;      // node -> its vehicle, or -1
    std::vector<int> positionOf_;  // node -> its position in that route
    std::int64_t total_ = 0;
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_COMMON_HPP

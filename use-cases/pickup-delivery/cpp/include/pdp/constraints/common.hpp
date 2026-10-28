#ifndef PDP_CONSTRAINTS_COMMON_HPP
#define PDP_CONSTRAINTS_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <vector>

#include "pdp/domain.hpp"
#include "pdp/score_calculator.hpp"
#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace pdp
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The seven PDPTW constraints, split into one small component each on the
// engine's DecomposedScoreCalculator — an optional assembly path NEXT TO
// ScoreCalculator::calculateScore and the monolithic incremental calculator,
// which stay the tuned default path.
//
// Unit choice: with the LIST descriptor the calculator's unit is one LIST,
// i.e. one vehicle; members are its nodes IN ROUTE ORDER. The descriptor
// exposes the unassigned pool as one extra list after the vehicles, so every
// per-route component steps over it (detail::isPool).
//
// Three rules are per-route sums and run as UnitConstraints. The three
// coupling rules relate a request's pickup to its dropoff — which may sit in
// different routes, which is the whole point of the rule — and
// unassignedRequests counts requests in no route at all, so those four are
// GlobalConstraints.
//
// A component never reads the Node shadow variables (vehicleIndex,
// arrivalTime): walkRoute below re-derives the arrival chain from the unit's
// own member order, which is what makes each cost a pure function of
// (solution, unit, members) as the engine's contract requires. The LIST path
// re-costs a touched route whole, so no delta contract is needed.
//
// Score levels: HardMediumSoft folded into the engine's HardSoftScore as
// `soft = medium * kMediumUnit + soft` (see domain.hpp), so the medium
// component carries that factor in its default weight. The three coupling
// rules default to ScoreCalculator::kCouplingPenalty, which is far above a
// typical lateness so the search repairs broken pairs before polishing time
// windows — and, being an ordinary weight here, is now a one-argument change.
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
// The level and the weight of each component are DEFAULTS — assembly-time
// metadata, not part of the cost. Override them per instance via the mutable
// `isHard` / `weight` / `exponent`.
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty; the calculator negates into the score. An assembly of all seven
// components at their default weights therefore scores bit-for-bit like
// ScoreCalculator::calculateScore.

using DecomposedCalculator = optimaru::DecomposedScoreCalculator<PdpRoutePlan>;
using ConstraintAssembly = optimaru::ConstraintAssembly<PdpRoutePlan>;
using PdpUnitConstraint = optimaru::UnitConstraint<PdpRoutePlan>;
using PdpGlobalConstraint = optimaru::GlobalConstraint<PdpRoutePlan>;

namespace detail
{

struct RouteMetrics
{
    Seconds driving = 0;
    Seconds endTime = 0;
    Seconds latency = 0;
};

// Is this list the virtual unassigned pool rather than a real vehicle?
inline bool isPool(const PdpRoutePlan &plan, int unit)
{
    return unit >= static_cast<int>(plan.vehicles.size());
}

// The arrival chain of one route, re-derived from its member order:
//   arrival      = previous departure + drivingTime(previous -> this)
//   departure    = max(arrival, minStartTime) + serviceDuration
// Calls onNode(nodeIndex, arrival) in order and returns the driving time of
// the whole route, home -> nodes... and back home only when the vehicle
// returns to its depot (0 for an empty route).
template <typename F>
inline Seconds walkRoute(const PdpRoutePlan &plan,
                         int vehicleIndex,
                         const std::vector<int> &members,
                         F &&onNode)
{
    if (members.empty())
        return 0;
    const Vehicle &vehicle = plan.vehicles[vehicleIndex];
    Seconds previousDeparture = vehicle.departureTime;
    int previousLocation = vehicle.homeLocationIndex;
    Seconds driving = 0;
    for (int nodeIndex : members)
    {
        const Node &node = plan.nodes[static_cast<std::size_t>(nodeIndex)];
        const Seconds leg
            = plan.drivingTime(previousLocation, node.locationIndex);
        const Seconds arrival = previousDeparture + leg;
        driving += leg;
        onNode(nodeIndex, arrival);
        previousDeparture
            = std::max(arrival, node.minStartTime) + node.serviceDuration;
        previousLocation = node.locationIndex;
    }
    driving += vehicle.homeLeg(plan.drivingSeconds, previousLocation);
    return driving;
}

inline RouteMetrics routeMetrics(const PdpRoutePlan &plan,
                                 int vehicleIndex,
                                 const std::vector<int> &members)
{
    const Vehicle &vehicle = plan.vehicles[vehicleIndex];
    RouteMetrics metrics{0, vehicle.departureTime, 0};
    if (members.empty())
        return metrics;
    Seconds previousDeparture = vehicle.departureTime;
    int previousLocation = vehicle.homeLocationIndex;
    for (int nodeIndex : members)
    {
        const Node &node = plan.nodes[static_cast<std::size_t>(nodeIndex)];
        const Seconds leg
            = plan.drivingTime(previousLocation, node.locationIndex);
        const Seconds arrival = previousDeparture + leg;
        metrics.driving += leg;
        metrics.latency
            += std::max<Seconds>(0, arrival - vehicle.departureTime);
        previousDeparture
            = std::max(arrival, node.minStartTime) + node.serviceDuration;
        previousLocation = node.locationIndex;
    }
    // An open route ends at its last stop's departure; a closed one after
    // the drive back home.
    const Seconds homeLeg
        = vehicle.homeLeg(plan.drivingSeconds, previousLocation);
    metrics.driving += homeLeg;
    metrics.endTime = previousDeparture + homeLeg;
    return metrics;
}

inline double haversineMeters(const Location &a, const Location &b)
{
    constexpr double earthRadius = 6371000.0;
    constexpr double pi = 3.14159265358979323846;
    const double lat1 = a.latitude * pi / 180.0;
    const double lat2 = b.latitude * pi / 180.0;
    const double dlat = lat2 - lat1;
    const double dlon = (b.longitude - a.longitude) * pi / 180.0;
    const double h = std::sin(dlat / 2.0) * std::sin(dlat / 2.0)
                     + std::cos(lat1) * std::cos(lat2) * std::sin(dlon / 2.0)
                           * std::sin(dlon / 2.0);
    return 2.0 * earthRadius * std::asin(std::sqrt(std::min(1.0, h)));
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Request-coupling global constraints
// ---------------------------------------------------------------------------
// Shared base of the three rules that judge a request's pickup against its
// dropoff. It tracks where each node currently sits and at which position —
// the solution's own shadow fields cannot be used, because on a leave
// notification they are already stale — and re-scores whole routes on every
// change, which keeps positions exact without a delta argument.
struct RequestCouplingConstraint : PdpGlobalConstraint
{
    using PdpGlobalConstraint::PdpGlobalConstraint;

    // The penalty units of one request given where its two nodes sit
    // (unit == -1 means "in no route").
    virtual std::int64_t requestCost(const PdpRoutePlan &plan,
                                     const Delivery &delivery,
                                     int pickupUnit,
                                     int pickupPosition,
                                     int dropoffUnit,
                                     int dropoffPosition) const = 0;

    void reset(const PdpRoutePlan &plan,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        plan_ = &plan;
        unitOf_.assign(plan.nodes.size(), -1);
        positionOf_.assign(plan.nodes.size(), -1);
        for (std::size_t unit = 0; unit < membersPerUnit.size(); ++unit)
        {
            if (detail::isPool(plan, static_cast<int>(unit)))
                continue;
            record(static_cast<int>(unit), membersPerUnit[unit]);
        }
        recompute();
    }

    void onUnitChanged(const PdpRoutePlan &plan,
                       int unit,
                       const std::vector<int> &members) override
    {
        plan_ = &plan;
        // Clear whatever used to sit in this unit, then record it anew.
        for (std::size_t node = 0; node < unitOf_.size(); ++node)
        {
            if (unitOf_[node] == unit)
            {
                unitOf_[node] = -1;
                positionOf_[node] = -1;
            }
        }
        if (!detail::isPool(plan, unit))
            record(unit, members);
        recompute();
    }

    double cost() const override { return static_cast<double>(total_); }

private:
    void record(int unit, const std::vector<int> &members)
    {
        for (std::size_t p = 0; p < members.size(); ++p)
        {
            const std::size_t node = static_cast<std::size_t>(members[p]);
            unitOf_[node] = unit;
            positionOf_[node] = static_cast<int>(p);
        }
    }

    void recompute()
    {
        total_ = 0;
        for (const Delivery &delivery : plan_->deliveries)
        {
            const std::size_t pickup
                = static_cast<std::size_t>(delivery.pickupNodeIndex);
            const std::size_t dropoff
                = static_cast<std::size_t>(delivery.dropoffNodeIndex);
            total_ += requestCost(*plan_,
                                  delivery,
                                  unitOf_[pickup],
                                  positionOf_[pickup],
                                  unitOf_[dropoff],
                                  positionOf_[dropoff]);
        }
    }

    const PdpRoutePlan *plan_ = nullptr;
    std::vector<int> unitOf_;      // node -> its vehicle, or -1
    std::vector<int> positionOf_;  // node -> its position in that route
    std::int64_t total_ = 0;
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_COMMON_HPP

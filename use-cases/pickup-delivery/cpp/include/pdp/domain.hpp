#ifndef PDP_DOMAIN_HPP
#define PDP_DOMAIN_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace pdp
{

// ---------------------------------------------------------------------------
// Pickup and Delivery Problem with Time Windows (PDPTW)
// ---------------------------------------------------------------------------
// Pickup-and-delivery domain: transport requests, each
// a (pickup, dropoff) pair of stops that must be served by ONE vehicle, pickup
// first, without ever exceeding the vehicle's capacity in between.
//
// The difference from plain vehicle routing is entirely in those coupling
// rules. A VRP visit is independent — it can go anywhere. A PDP node cannot:
// it drags its partner along. That is why this is a separate use case rather
// than a constraint bolted onto vehicle-routing, and why the solver registers
// pair-aware move and ruin operators (see solver.hpp) instead of relying on
// the engine's element-granular built-ins alone.

using Seconds = std::int64_t;

constexpr Seconds NULL_TIME = -1;

// ---------------------------------------------------------------------------
// HardMediumSoftScore
// ---------------------------------------------------------------------------
// Same convention and packing as the vehicle-routing port: the engine speaks
// HardSoftScore(int64 hard, double soft), so medium and soft share the soft
// level with a 10^9 multiplier. See vrp/domain.hpp for the magnitude analysis;
// the bounds are identical here (|soft| = driving seconds, |medium| = service
// minutes of unassigned requests).
constexpr std::int64_t kMediumUnit = 1000000000LL;  // 10^9

struct Score
{
    std::int64_t hard = 0;
    std::int64_t medium = 0;
    std::int64_t soft = 0;

    bool isFeasible() const { return hard >= 0; }

    bool operator<(const Score &o) const
    {
        if (hard != o.hard)
            return hard < o.hard;
        if (medium != o.medium)
            return medium < o.medium;
        return soft < o.soft;
    }
    bool operator>(const Score &o) const { return o < *this; }
    bool operator<=(const Score &o) const { return !(o < *this); }
    bool operator>=(const Score &o) const { return !(*this < o); }
    bool operator==(const Score &o) const
    {
        return hard == o.hard && medium == o.medium && soft == o.soft;
    }

    std::string toString() const
    {
        return std::to_string(hard) + "hard/" + std::to_string(medium)
               + "medium/" + std::to_string(soft) + "soft";
    }
};

inline optimaru::HardSoftScore toEngineScore(const Score &s)
{
    return {s.hard, static_cast<double>(s.medium * kMediumUnit + s.soft)};
}

inline Score fromEngineScore(const optimaru::HardSoftScore &e)
{
    const auto combined = static_cast<std::int64_t>(std::llround(e.soft));
    return {e.hard, combined / kMediumUnit, combined % kMediumUnit};
}

struct Location
{
    double latitude = 0.0;
    double longitude = 0.0;
    int index = -1;
};

// ---------------------------------------------------------------------------
// Node — one stop (a pickup or a dropoff)
// ---------------------------------------------------------------------------
// The planning *elements* are nodes, not requests: the engine's list variable
// holds a sequence of stops. `deliveryIndex` links a node back to the request
// it belongs to, and `pickup` says which half it is.
struct Node
{
    std::string id;
    int locationIndex = -1;
    Seconds minStartTime = 0;  // time window open
    Seconds maxEndTime = 0;    // time window close
    Seconds serviceDuration = 0;
    int deliveryIndex = -1;  // which Delivery this node belongs to
    bool pickup = false;     // true = pickup, false = dropoff

    // --- shadow (recomputed by ScoreCalculator::updateShadowVariables) ---
    int vehicleIndex = -1;
    Seconds arrivalTime = NULL_TIME;
};

// ---------------------------------------------------------------------------
// Delivery — one transport request
// ---------------------------------------------------------------------------
// A delivery is a (pickup, dropoff) pair moving `demand` units. The
// demand is added to the vehicle's load at the pickup and removed at the
// dropoff, so the binding capacity check is on the running load *between* the
// two, not on the route total (a vehicle can serve far more than its capacity
// over a day as long as it never carries too much at once).
struct Delivery
{
    std::string id;
    int pickupNodeIndex = -1;
    int dropoffNodeIndex = -1;
    int demand = 0;
    // When the request was placed, on the plan's clock: negative if it has
    // already been waiting when the plan starts.
    Seconds orderTime = 0;
    // The vehicle this request must ride on, or -1 for any.
    int requiredVehicleIndex = -1;
};

// ---------------------------------------------------------------------------
// Vehicle
// ---------------------------------------------------------------------------
struct Vehicle
{
    std::string id;
    int capacity = 0;
    int homeLocationIndex = -1;
    Seconds departureTime = 0;
    // Optional route policies. Non-positive means disabled.
    Seconds maxTravelTime = 0;
    Seconds maxRouteDuration = 0;
    Seconds maxEndTime = 0;
    int maxStops = 0;
    double minLoadRatio = 0.0;
    double fixedCost = 0.0;
    double maxRouteRadiusMeters = 0.0;
    // Does the route end back at home? Off by default: the route is open and
    // ends at its last stop, so the last-stop -> home leg counts toward
    // neither driving time nor the route end time. On, the leg is charged
    // like a classic round trip.
    bool returnToDepot = false;
    std::vector<int> nodeIndices;  // ordered route of stops
    // Stops the route must start with, in order. A solve keeps any route already
    // in nodeIndices as its starting point, so these are normally its prefix.
    std::vector<int> pinnedNodeIndices;

    // The driving seconds from the last stop back home, or 0 for an open
    // route. Every scoring path charges the closing leg through this one
    // helper so the monolithic, incremental and plugin costs cannot drift.
    Seconds homeLeg(const std::vector<std::vector<Seconds>> &drivingSeconds,
                    int lastLocation) const
    {
        return returnToDepot
                   ? drivingSeconds[static_cast<std::size_t>(lastLocation)]
                                   [static_cast<std::size_t>(homeLocationIndex)]
                   : 0;
    }
};

// ---------------------------------------------------------------------------
// PdpRoutePlan  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct PdpRoutePlan
{
    std::string name;
    std::vector<Location> locations;
    std::vector<Vehicle> vehicles;
    std::vector<Node> nodes;
    std::vector<Delivery> deliveries;

    // The unassigned pool, exactly as in vehicle-routing: one extra virtual
    // list at index vehicles.size() so that "unassigned" is a real place the
    // engine's moves can reach. A request is unassigned when BOTH its nodes
    // sit here; one node here and one on a route is a half-assigned request,
    // which is hard-penalized (see ScoreCalculator).
    std::vector<int> unassignedNodeIndices;

    std::vector<std::vector<Seconds>> drivingSeconds;

    Score score;

    Seconds drivingTime(int fromLocation, int toLocation) const
    {
        return drivingSeconds[fromLocation][toLocation];
    }

    // The partner node of a pickup/dropoff.
    int partnerOf(int nodeIndex) const
    {
        const Node &node = nodes[static_cast<std::size_t>(nodeIndex)];
        const Delivery &delivery
            = deliveries[static_cast<std::size_t>(node.deliveryIndex)];
        return node.pickup ? delivery.dropoffNodeIndex
                           : delivery.pickupNodeIndex;
    }

    void rebuildUnassignedPool()
    {
        std::vector<char> routed(nodes.size(), 0);
        for (const Vehicle &vehicle : vehicles)
        {
            for (int nodeIndex : vehicle.nodeIndices)
                routed[static_cast<std::size_t>(nodeIndex)] = 1;
        }
        unassignedNodeIndices.clear();
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (!routed[i])
                unassignedNodeIndices.push_back(static_cast<int>(i));
        }
    }
};

}  // namespace pdp

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization
// ---------------------------------------------------------------------------
// Same shape as vehicle-routing's: vehicles are the list owners, nodes are the
// elements, and list index vehicles.size() is the virtual unassigned pool.
template <> struct optimaru::SolutionDescriptor<pdp::PdpRoutePlan>
{
    static constexpr optimaru::VariableKind kind = optimaru::VariableKind::LIST;

    static int unassignedList(const pdp::PdpRoutePlan &s)
    {
        return static_cast<int>(s.vehicles.size());
    }

    static int listCount(const pdp::PdpRoutePlan &s)
    {
        return static_cast<int>(s.vehicles.size()) + 1;
    }
    static int elementCount(const pdp::PdpRoutePlan &s)
    {
        return static_cast<int>(s.nodes.size());
    }

    static std::vector<int> &listOf(pdp::PdpRoutePlan &s, int listIndex)
    {
        return listIndex == unassignedList(s)
                   ? s.unassignedNodeIndices
                   : s.vehicles[listIndex].nodeIndices;
    }
    static const std::vector<int> &listOf(const pdp::PdpRoutePlan &s,
                                          int listIndex)
    {
        return listIndex == unassignedList(s)
                   ? s.unassignedNodeIndices
                   : s.vehicles[listIndex].nodeIndices;
    }

    static int listSize(const pdp::PdpRoutePlan &s, int listIndex)
    {
        return static_cast<int>(listOf(s, listIndex).size());
    }
    static int
    getElement(const pdp::PdpRoutePlan &s, int listIndex, int position)
    {
        return listOf(s, listIndex)[static_cast<std::size_t>(position)];
    }
    static void insertElement(pdp::PdpRoutePlan &s,
                              int listIndex,
                              int position,
                              int element)
    {
        std::vector<int> &list = listOf(s, listIndex);
        list.insert(list.begin() + position, element);
    }
    static int removeElement(pdp::PdpRoutePlan &s, int listIndex, int position)
    {
        std::vector<int> &list = listOf(s, listIndex);
        const int element = list[static_cast<std::size_t>(position)];
        list.erase(list.begin() + position);
        return element;
    }
    static void
    reverseSegment(pdp::PdpRoutePlan &s, int listIndex, int first, int last)
    {
        std::vector<int> &list = listOf(s, listIndex);
        std::reverse(list.begin() + first, list.begin() + last + 1);
    }
};

#endif  // PDP_DOMAIN_HPP

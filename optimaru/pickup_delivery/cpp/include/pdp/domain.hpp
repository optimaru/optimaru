#ifndef PDP_DOMAIN_HPP
#define PDP_DOMAIN_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/score/hard_medium_soft_score.hpp"
#include "optimaru/score/levels.hpp"

namespace pdp {

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
// request-level move and ruin operators (see solver.hpp) instead of relying on
// the engine's element-granular built-ins alone.

using Seconds = std::int64_t;

constexpr Seconds NULL_TIME = -1;

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// Same convention and packing as the vehicle-routing use case: the engine speaks
// Score(int64 hard, double soft), so medium and soft share the soft
// level with a 10^9 multiplier. See vrp/domain.hpp for the magnitude analysis;
// the bounds are identical here (|soft| = driving seconds, |medium| = service
// minutes of unassigned requests).
//
// The fold is declared ONCE, here: toEngineScore below packs the monolithic
// Score with it, and the MediumFoldOf<PdpRoutePlan> specialization at the end
// of this header hands the same constant to the decomposed calculator, which
// keeps a separate medium accumulator. A medium constraint's weight is
// therefore in medium units (unassignedRequestsCost: 1), never 10^9.
constexpr std::int64_t kMediumUnit = 1000000000LL;  // 10^9
constexpr optimaru::MediumFold kMediumFold =
        optimaru::MediumFold::into_soft(static_cast<double>(kMediumUnit));

using Score = optimaru::HardMediumSoftScore;

// On the medium or the soft level of Solver::Config::target_score: no floor on
// the soft side, so only the hard level is compared. stop_at_first_feasible
// passes (0, kNoLevelFloor, kNoLevelFloor).
using optimaru::kNoLevelFloor;

inline optimaru::Score toEngineScore(const Score& s) {
    return optimaru::to_engine_score(kMediumFold, s);
}

// A best-score limit (Solver::Config::target_score) in engine terms.
inline optimaru::Score toEngineScoreLimit(const Score& limit) {
    return optimaru::to_engine_score_limit(kMediumFold, limit);
}

// Inverse of toEngineScore.
inline Score fromEngineScore(const optimaru::Score& e) {
    return optimaru::from_engine_score(kMediumFold, e);
}

struct Location {
    double latitude = 0.0;
    double longitude = 0.0;
    int index = -1;
};

// ---------------------------------------------------------------------------
// Node — one stop (a pickup or a dropoff)
// ---------------------------------------------------------------------------
// The list *elements* are nodes, not requests: the engine's list variable
// holds a sequence of stops. `deliveryIndex` links a node back to the request
// it belongs to, and `pickup` says which half it is.
struct Node {
    std::string id;
    int locationIndex = -1;
    Seconds minStartTime = 0;  // time window open
    Seconds maxEndTime = 0;    // time window close
    Seconds serviceDuration = 0;
    int deliveryIndex = -1;  // which Delivery this node belongs to
    bool pickup = false;     // true = pickup, false = dropoff

    // --- derived (recomputed by ScoreCalculator::updateShadowVariables) ---
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
struct Delivery {
    std::string id;
    int pickupNodeIndex = -1;
    int dropoffNodeIndex = -1;
    int demand = 0;
    // When the request was placed, on the plan's clock: negative if it has
    // already been waiting when the plan starts.
    Seconds orderTime = 0;
    // The vehicle this request must ride on, or -1 for any.
    int requiredVehicleIndex = -1;
    // The group this request travels in, or -1 for none. The requests of one
    // group ride one vehicle together, or all wait (requestGroupCost): several
    // orders the input has merged into one job.
    int groupIndex = -1;
};

// ---------------------------------------------------------------------------
// Vehicle
// ---------------------------------------------------------------------------
struct Vehicle {
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
    Seconds homeLeg(const std::vector<std::vector<Seconds>>& drivingSeconds,
                    int lastLocation) const {
        return returnToDepot ? drivingSeconds[static_cast<std::size_t>(lastLocation)]
                                             [static_cast<std::size_t>(homeLocationIndex)]
                             : 0;
    }
};

// ---------------------------------------------------------------------------
// PdpRoutePlan  (the solution)
// ---------------------------------------------------------------------------
struct PdpRoutePlan {
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

    Seconds drivingTime(int fromLocation, int toLocation) const {
        return drivingSeconds[fromLocation][toLocation];
    }

    // The partner node of a pickup/dropoff.
    int partnerOf(int nodeIndex) const {
        const Node& node = nodes[static_cast<std::size_t>(nodeIndex)];
        const Delivery& delivery = deliveries[static_cast<std::size_t>(node.deliveryIndex)];
        return node.pickup ? delivery.dropoffNodeIndex : delivery.pickupNodeIndex;
    }

    // Whether some group (Delivery::groupIndex) holds two requests or more. A
    // request alone in its group changes nothing.
    bool hasRequestGroups() const {
        std::unordered_set<int> seen;
        for (const Delivery& delivery : deliveries) {
            if (delivery.groupIndex >= 0 && !seen.insert(delivery.groupIndex).second) return true;
        }
        return false;
    }

    void rebuildUnassignedPool() {
        std::vector<char> routed(nodes.size(), 0);
        for (const Vehicle& vehicle : vehicles) {
            for (int nodeIndex : vehicle.nodeIndices)
                routed[static_cast<std::size_t>(nodeIndex)] = 1;
        }
        unassignedNodeIndices.clear();
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (!routed[i]) unassignedNodeIndices.push_back(static_cast<int>(i));
        }
    }
};

}  // namespace pdp

// ---------------------------------------------------------------------------
// SolutionTraits specialization
// ---------------------------------------------------------------------------
// Same shape as vehicle-routing's: vehicles are the list owners, nodes are the
// elements, and list index vehicles.size() is the virtual unassigned pool.
template <>
struct optimaru::SolutionTraits<pdp::PdpRoutePlan> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Sequence;

    static int unassigned_list(const pdp::PdpRoutePlan& s) {
        return static_cast<int>(s.vehicles.size());
    }

    static int list_count(const pdp::PdpRoutePlan& s) {
        return static_cast<int>(s.vehicles.size()) + 1;
    }
    static int element_count(const pdp::PdpRoutePlan& s) {
        return static_cast<int>(s.nodes.size());
    }

    static std::vector<int>& list_of(pdp::PdpRoutePlan& s, int listIndex) {
        return listIndex == unassigned_list(s) ? s.unassignedNodeIndices
                                               : s.vehicles[listIndex].nodeIndices;
    }
    static const std::vector<int>& list_of(const pdp::PdpRoutePlan& s, int listIndex) {
        return listIndex == unassigned_list(s) ? s.unassignedNodeIndices
                                               : s.vehicles[listIndex].nodeIndices;
    }

    static int list_size(const pdp::PdpRoutePlan& s, int listIndex) {
        return static_cast<int>(list_of(s, listIndex).size());
    }
    static int element_at(const pdp::PdpRoutePlan& s, int listIndex, int position) {
        return list_of(s, listIndex)[static_cast<std::size_t>(position)];
    }
    static void insert_element(pdp::PdpRoutePlan& s, int listIndex, int position, int element) {
        std::vector<int>& list = list_of(s, listIndex);
        list.insert(list.begin() + position, element);
    }
    static int remove_element(pdp::PdpRoutePlan& s, int listIndex, int position) {
        std::vector<int>& list = list_of(s, listIndex);
        const int element = list[static_cast<std::size_t>(position)];
        list.erase(list.begin() + position);
        return element;
    }
    static void reverse_segment(pdp::PdpRoutePlan& s, int listIndex, int first, int last) {
        std::vector<int>& list = list_of(s, listIndex);
        std::reverse(list.begin() + first, list.begin() + last + 1);
    }
};

// ---------------------------------------------------------------------------
// Medium fold registration
// ---------------------------------------------------------------------------
// The decomposed calculator folds its medium accumulator with the domain's
// kMediumFold (see the Score adapter above).
template <>
struct optimaru::MediumFoldOf<pdp::PdpRoutePlan> {
    static constexpr optimaru::MediumFold value = pdp::kMediumFold;
};

#endif  // PDP_DOMAIN_HPP

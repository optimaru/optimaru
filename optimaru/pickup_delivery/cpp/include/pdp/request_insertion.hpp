#ifndef PDP_REQUEST_INSERTION_HPP
#define PDP_REQUEST_INSERTION_HPP

#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "pdp/domain.hpp"

namespace pdp {

// ---------------------------------------------------------------------------
// Request insertion
// ---------------------------------------------------------------------------
// Where a whole request goes: the pickup and the dropoff are placed together,
// on one vehicle, pickup first. This is the unit every request-level operator
// (construction, the request moves in request_moves.hpp) works in, so none of
// them ever passes through a half-routed or split request.
//
// The search is the classic two-slot scan: for every pickup slot, walk the
// dropoff slot forward and carry the schedule along. A RouteTimeline keeps what
// makes one candidate O(1): each stop's departure, the latest arrival that still
// keeps the rest of the route on time, and the load on board.
//
// What counts as feasible is read from the plan, not from the objective: the
// running load against Vehicle::capacity, every stop's time window, and the
// vehicle limits that are set (maxEndTime, maxRouteDuration, maxTravelTime,
// maxStops, the pinned prefix, Delivery::requiredVehicleIndex). Among the
// feasible slots the cheapest in added driving time wins. The objective still
// judges the result: an operator applies what this search proposes through the
// editor, and the acceptor sees the real score.

// One way to add a request to a route.
struct RequestInsertion {
    int vehicle = -1;
    // The pickup's slot in the route as it is.
    int pickupPosition = -1;
    // The dropoff's slot once the pickup is in, so it is past pickupPosition.
    int dropoffPosition = -1;
    Seconds addedTravel = std::numeric_limits<Seconds>::max();

    bool found() const { return vehicle >= 0; }
};

// Blinking (Christiaens & Vanden Berghe, "Slack Induction by String Removals",
// 2020): a repair skips each candidate slot with a small probability, so the
// same hole is not rebuilt the same way every time. The gap to the next skip is
// drawn once, so a candidate costs one decrement, not one random number.
class Blinker {
public:
    // Never skips.
    Blinker() = default;
    // Skips each candidate with probability `rate` (0 or less: never).
    Blinker(double rate, std::mt19937_64& rng);

    bool skip() {
        if (rng_ == nullptr || --remaining_ > 0) return false;
        draw();
        return true;
    }

private:
    void draw();

    std::mt19937_64* rng_ = nullptr;
    double logKeep_ = 0.0;
    std::int64_t remaining_ = 0;
};

// The schedule of one route, in the form the insertion scan reads.
class RouteTimeline {
public:
    // Recomputes the timeline of `route` driven by `vehicleIndex`. `route` is
    // any stop sequence: the vehicle's own, or a working copy of it.
    void rebuild(const PdpRoutePlan& plan, int vehicleIndex, const std::vector<int>& route);

    // Offers every feasible slot pair of `delivery` in this route and keeps the
    // cheapest in `best` (a slot pair must be strictly cheaper to replace it).
    void improve(const PdpRoutePlan& plan, const Delivery& delivery, Blinker& blinker,
                 RequestInsertion& best) const;

    // The driving seconds the route saves when the stops at these two
    // positions are taken out (pickupPosition < dropoffPosition).
    Seconds removalGain(const PdpRoutePlan& plan, int pickupPosition, int dropoffPosition) const;

    int size() const { return static_cast<int>(stops_.size()); }
    // Driving seconds of the route, the leg home included when it is driven.
    Seconds travel() const { return travel_; }

private:
    struct Stop {
        int location = -1;
        Seconds minStart = 0;
        Seconds service = 0;
        Seconds due = 0;        // latest arrival of the stop's own window
        Seconds departure = 0;  // when the vehicle leaves the stop
        Seconds latest = 0;     // latest arrival keeping this and every later stop on time
        std::int64_t load = 0;  // on board when leaving the stop
    };

    // Driving seconds from `location` to the stop at `position`, or to the
    // route's end: home when the route is closed, nothing when it is open.
    Seconds legTo(const PdpRoutePlan& plan, int location, int position) const;

    int vehicle_ = -1;
    std::vector<Stop> stops_;
    Seconds travel_ = 0;
    Seconds endLimit_ = 0;  // latest route end the vehicle allows
};

// The timelines of every route of a plan, and the search over them.
class PlanTimelines {
public:
    // Timelines of the plan's own routes.
    void rebuild(const PdpRoutePlan& plan);
    // Refreshes one route after it changed.
    void rebuildRoute(const PdpRoutePlan& plan, int vehicleIndex, const std::vector<int>& route);

    const RouteTimeline& of(int vehicleIndex) const {
        return timelines_[static_cast<std::size_t>(vehicleIndex)];
    }

    // The vehicles worth offering `delivery` to: its required vehicle if it has
    // one, otherwise every vehicle with a route and, of the idle vehicles that
    // would serve it identically, only the first.
    void candidateVehicles(const PdpRoutePlan& plan, const Delivery& delivery,
                           std::vector<int>& out) const;

    // The cheapest feasible insertion of `delivery` over every vehicle.
    RequestInsertion best(const PdpRoutePlan& plan, const Delivery& delivery,
                          Blinker& blinker) const;

private:
    std::vector<RouteTimeline> timelines_;
    // Vehicles with the same class serve a request identically while idle:
    // equal limits and equal driving times from and to their home.
    std::vector<int> vehicleClass_;
    int classCount_ = 0;
    mutable std::vector<char> idleClassSeen_;
    mutable std::vector<int> candidateScratch_;
};

}  // namespace pdp

#endif  // PDP_REQUEST_INSERTION_HPP

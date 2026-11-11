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
//
// Requests the input has put in one group (Delivery::groupIndex) go in as one
// unit, all on one vehicle: RequestGroups says which requests travel together
// and UnitPlanner finds their slots on a route.

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
// drawn once, so a candidate costs one decrement, not one random number. The
// scan only blinks once it holds a feasible slot, so blinking never leaves a
// request without one.
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
    // While `best` is empty no slot is blinked.
    void improve(const PdpRoutePlan& plan, const Delivery& delivery, Blinker& blinker,
                 RequestInsertion& best) const;

    // The driving seconds the route saves when the stops at these two
    // positions are taken out (pickupPosition < dropoffPosition).
    Seconds removalGain(const PdpRoutePlan& plan, int pickupPosition, int dropoffPosition) const;

    int size() const { return static_cast<int>(stops_.size()); }
    // The vehicle whose route this is.
    int vehicle() const { return vehicle_; }
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
//
// One object serves one plan: which vehicles serve a request identically is
// worked out on the first rebuild() and kept, since a solve does not change
// its vehicles. Use a new object for another plan.
class PlanTimelines {
public:
    // Timelines of the plan's own routes.
    void rebuild(const PdpRoutePlan& plan);
    // Refreshes one route after it changed.
    void rebuildRoute(const PdpRoutePlan& plan, int vehicleIndex, const std::vector<int>& route);

    const RouteTimeline& of(int vehicleIndex) const {
        return timelines_[static_cast<std::size_t>(vehicleIndex)];
    }

    // The vehicles worth offering a request to: `requiredVehicle` alone when
    // the request is bound to one (0 or more), otherwise every vehicle with a
    // route and, of the idle vehicles that would serve it identically, only the
    // first.
    void candidateVehicles(const PdpRoutePlan& plan, int requiredVehicle,
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

// ---------------------------------------------------------------------------
// Request groups
// ---------------------------------------------------------------------------
// Which requests travel together. The requests of one group
// (Delivery::groupIndex) ride one vehicle or all wait, so the request-level
// operators move them as one unit: they leave their routes together and go
// back on one vehicle, or stay out together.
//
// A unit is named by its first request, its leader; a request in no group is a
// unit of one and its own leader. A request with a pinned stop cannot leave its
// route: it is a unit of one that stays, and the rest of its group is bound to
// its vehicle, as it is to the required vehicle of any request of the group.
class RequestGroups {
public:
    void build(const PdpRoutePlan& plan);

    // Whether some unit holds more than one request.
    bool any() const { return any_; }
    int leaderOf(int deliveryIndex) const {
        return leader_[static_cast<std::size_t>(deliveryIndex)];
    }
    // The requests of a unit: start at its leader and follow nextOf() to -1.
    int nextOf(int deliveryIndex) const { return next_[static_cast<std::size_t>(deliveryIndex)]; }
    int sizeOf(int leader) const { return size_[static_cast<std::size_t>(leader)]; }
    // The one vehicle the request may ride (its own required vehicle, or the
    // one its group is bound to), or -1 for any.
    int vehicleOf(int deliveryIndex) const {
        return vehicle_[static_cast<std::size_t>(deliveryIndex)];
    }
    // The vehicle that pins one of the request's stops, or -1.
    int pinnedOn(int deliveryIndex) const {
        return pinnedOn_[static_cast<std::size_t>(deliveryIndex)];
    }
    bool pinned(int deliveryIndex) const { return pinnedOn(deliveryIndex) >= 0; }

private:
    std::vector<int> leader_;
    std::vector<int> next_;
    std::vector<int> size_;
    std::vector<int> vehicle_;
    std::vector<int> pinnedOn_;
    bool any_ = false;
};

// Finds room for several requests on one route: they go in one after another,
// each at the cheapest slot pair the route has once the ones before it are in.
class UnitPlanner {
public:
    // Plans `requests` onto `route`, whose timeline is `timeline`. Appends one
    // insertion per request to `steps` (each reads the route with the earlier
    // ones in) and returns the driving seconds they add together in
    // `addedTravel`. Returns false, with `steps` as it was, when one of them
    // has no slot.
    bool plan(const PdpRoutePlan& plan, const RouteTimeline& timeline,
              const std::vector<int>& route, const std::vector<int>& requests, Blinker& blinker,
              std::vector<RequestInsertion>& steps, Seconds& addedTravel);

private:
    std::vector<int> route_;
    RouteTimeline timeline_;
};

}  // namespace pdp

#endif  // PDP_REQUEST_INSERTION_HPP

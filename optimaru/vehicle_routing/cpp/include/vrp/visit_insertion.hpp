#ifndef VRP_VISIT_INSERTION_HPP
#define VRP_VISIT_INSERTION_HPP

#include <cstdint>
#include <limits>
#include <vector>

#include "vrp/domain.hpp"

namespace vrp {

// ---------------------------------------------------------------------------
// Visit insertion
// ---------------------------------------------------------------------------
// Where a visit goes into a route, judged from the plan and not from the
// objective: the route's demand against Vehicle::capacity, every visit's time
// window, and the vehicle limits that are set (maxEndTime, maxRouteDuration,
// maxTravelTime, maxStops). Among the feasible slots the cheapest in added
// driving time wins. A RouteTimeline keeps what makes one slot O(1): each
// stop's departure and the latest arrival that still keeps the rest of the
// route on time.
//
// This is what the group operators (group_moves.hpp) and the construction of
// groups place visits with: the visits of one group (Visit::groupIndex) go
// onto one vehicle together, in an order that respects
// Visit::afterVisitIndices. The objective still judges the result: an operator
// applies what this search proposes through the editor, and the acceptor sees
// the real score.

// One way to add a visit to a route.
struct VisitInsertion {
    int vehicle = -1;
    // The slot in the route as it is: the visit goes in front of the stop now
    // at this position, or behind the last one.
    int position = -1;
    Seconds addedTravel = std::numeric_limits<Seconds>::max();

    bool found() const { return vehicle >= 0; }
};

// The schedule of one route, in the form the insertion scan reads.
class RouteTimeline {
public:
    // Recomputes the timeline of `route` driven by `vehicleIndex`. `route` is
    // any visit sequence: the vehicle's own, or a working copy of it.
    void rebuild(const VehicleRoutePlan& plan, int vehicleIndex, const std::vector<int>& route);

    // Offers the slots `first` .. `last` (clamped to 0 .. size()) and keeps the
    // cheapest feasible one in `best`; a slot must be strictly cheaper to
    // replace what `best` holds.
    void improve(const VehicleRoutePlan& plan, const Visit& visit, int first, int last,
                 VisitInsertion& best) const;

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
    };

    int vehicle_ = -1;
    std::vector<Stop> stops_;
    Seconds travel_ = 0;
    Seconds endLimit_ = 0;     // latest route end the vehicle allows
    std::int64_t demand_ = 0;  // of the visits on the route
};

// ---------------------------------------------------------------------------
// Visit groups
// ---------------------------------------------------------------------------
// Which visits travel together. The visits of one group (Visit::groupIndex)
// ride one vehicle or all wait, so the group operators move them as one unit:
// they leave their routes together and go back on one vehicle, or stay out
// together.
//
// A unit is named by its first visit, its leader; a visit in no group (or
// alone in its group) is a unit of one and its own leader. A unit lists its
// visits so that none comes before one it has to be served after
// (Visit::afterVisitIndices), which is the order they are put back in.
class VisitGroups {
public:
    // The plan's groups and order have been checked (checkVisitGroups in
    // solver.cpp): every visit a visit comes after is in its group, and the
    // order has no cycle.
    void build(const VehicleRoutePlan& plan);

    // Whether some unit holds more than one visit.
    bool any() const { return !groups_.empty(); }
    // The leaders of the units of more than one visit.
    const std::vector<int>& groups() const { return groups_; }
    int leaderOf(int visitIndex) const { return leader_[static_cast<std::size_t>(visitIndex)]; }
    // The visits of a unit: start at its leader and follow nextOf() to -1.
    int nextOf(int visitIndex) const { return next_[static_cast<std::size_t>(visitIndex)]; }
    int sizeOf(int leader) const { return size_[static_cast<std::size_t>(leader)]; }
    // The visits that have to be served after this one.
    const std::vector<int>& followersOf(int visitIndex) const {
        return followers_[static_cast<std::size_t>(visitIndex)];
    }

private:
    std::vector<int> leader_;
    std::vector<int> next_;
    std::vector<int> size_;
    std::vector<int> groups_;
    std::vector<std::vector<int>> followers_;
};

// One visit's place in a unit's plan. The slot reads the route with the plan's
// earlier steps in.
struct UnitStep {
    int visit = -1;
    VisitInsertion slot;
};

// Finds room for several visits on one route: they go in one after another, in
// the order given, each at the cheapest slot the route has once the ones before
// it are in. A visit only gets the slots behind the visits it comes after and
// in front of the ones that come after it, as far as they are on the route.
//
// One plan costs a walk of the route per visit, so this is for groups of a
// handful of visits.
class UnitPlanner {
public:
    // Plans `visits` onto `route`, whose timeline is `timeline`. `visits` names
    // no visit before one it comes after. Appends one step per visit to
    // `steps`, in the order to apply them, and returns the driving seconds they
    // add together in `addedTravel`. Returns false, with `steps` as it was,
    // when one of them has no slot.
    bool plan(const VehicleRoutePlan& plan, const VisitGroups& groups,
              const RouteTimeline& timeline, const std::vector<int>& route,
              const std::vector<int>& visits, std::vector<UnitStep>& steps, Seconds& addedTravel);

private:
    std::vector<int> route_;
    RouteTimeline timeline_;
    // Where each visit of the working route sits, valid while its stamp is the
    // current epoch.
    std::vector<int> position_;
    std::vector<std::uint64_t> stamp_;
    std::uint64_t epoch_ = 0;
};

}  // namespace vrp

#endif  // VRP_VISIT_INSERTION_HPP

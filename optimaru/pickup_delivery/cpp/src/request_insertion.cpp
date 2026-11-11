#include "pdp/request_insertion.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pdp {

namespace {

// Far enough from the int64 range that a few subtractions never wrap.
constexpr Seconds kNoLimit = std::numeric_limits<Seconds>::max() / 4;
constexpr Seconds kNever = std::numeric_limits<Seconds>::min() / 4;
// More candidates than any solve scans: a blinker this far from its next skip
// never skips.
constexpr std::int64_t kLongestGap = 1'000'000'000'000'000'000;

// Whether two vehicles serve any request identically while they are idle:
// the same limits, and the same driving times between their home and every
// stop. (Two vehicles of one depot usually have a location entry each.)
bool sameService(const PdpRoutePlan& plan, const Vehicle& a, const Vehicle& b) {
    if (a.capacity != b.capacity || a.departureTime != b.departureTime ||
        a.maxTravelTime != b.maxTravelTime || a.maxRouteDuration != b.maxRouteDuration ||
        a.maxEndTime != b.maxEndTime || a.maxStops != b.maxStops ||
        a.minLoadRatio != b.minLoadRatio || a.fixedCost != b.fixedCost ||
        a.maxRouteRadiusMeters != b.maxRouteRadiusMeters || a.returnToDepot != b.returnToDepot ||
        !a.pinnedNodeIndices.empty() || !b.pinnedNodeIndices.empty()) {
        return false;
    }
    const auto homeA = static_cast<std::size_t>(a.homeLocationIndex);
    const auto homeB = static_cast<std::size_t>(b.homeLocationIndex);
    if (homeA == homeB) return true;
    if (homeA < plan.locations.size() && homeB < plan.locations.size() &&
        (plan.locations[homeA].latitude != plan.locations[homeB].latitude ||
         plan.locations[homeA].longitude != plan.locations[homeB].longitude)) {
        return false;
    }
    for (const Node& node : plan.nodes) {
        const auto stop = static_cast<std::size_t>(node.locationIndex);
        if (plan.drivingSeconds[homeA][stop] != plan.drivingSeconds[homeB][stop] ||
            plan.drivingSeconds[stop][homeA] != plan.drivingSeconds[stop][homeB]) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Blinker
// ---------------------------------------------------------------------------

Blinker::Blinker(double rate, std::mt19937_64& rng) {
    // Written so that a NaN rate, like one of 0 or less, never skips.
    if (!(rate > 0.0)) return;
    rng_ = &rng;
    logKeep_ = std::log1p(-std::min(rate, 0.999999));
    draw();
}

void Blinker::draw() {
    // A geometric gap: the candidates up to and including the next skipped one.
    const double unit = (static_cast<double>((*rng_)() >> 11) + 1.0) * 0x1.0p-53;  // (0, 1]
    // With a rate near 0 the gap outgrows any scan, and the int64 range with
    // it: hold it at kLongestGap (the comparison is false for a NaN too).
    const double gap = std::log(unit) / logKeep_;
    remaining_ = gap < static_cast<double>(kLongestGap) ? 1 + static_cast<std::int64_t>(gap)
                                                        : kLongestGap;
}

// ---------------------------------------------------------------------------
// RouteTimeline
// ---------------------------------------------------------------------------

void RouteTimeline::rebuild(const PdpRoutePlan& plan, int vehicleIndex,
                            const std::vector<int>& route) {
    const Vehicle& vehicle = plan.vehicles[static_cast<std::size_t>(vehicleIndex)];
    vehicle_ = vehicleIndex;
    stops_.resize(route.size());
    travel_ = 0;
    endLimit_ = kNoLimit;
    if (vehicle.maxEndTime > 0) endLimit_ = vehicle.maxEndTime;
    if (vehicle.maxRouteDuration > 0) {
        endLimit_ = std::min(endLimit_, vehicle.departureTime + vehicle.maxRouteDuration);
    }
    if (route.empty()) return;

    Seconds departure = vehicle.departureTime;
    int location = vehicle.homeLocationIndex;
    std::int64_t load = 0;
    for (std::size_t i = 0; i < route.size(); ++i) {
        const Node& node = plan.nodes[static_cast<std::size_t>(route[i])];
        const Delivery& delivery = plan.deliveries[static_cast<std::size_t>(node.deliveryIndex)];
        Stop& stop = stops_[i];
        stop.location = node.locationIndex;
        stop.minStart = node.minStartTime;
        stop.service = node.serviceDuration;
        stop.due = node.maxEndTime - node.serviceDuration;
        const Seconds leg = plan.drivingTime(location, stop.location);
        travel_ += leg;
        departure = std::max(departure + leg, stop.minStart) + stop.service;
        stop.departure = departure;
        load += node.pickup ? delivery.demand : -delivery.demand;
        stop.load = load;
        location = stop.location;
    }
    const Seconds homeLeg = vehicle.homeLeg(plan.drivingSeconds, location);
    travel_ += homeLeg;

    // Backwards: how late each stop may be reached. A stop never starts before
    // its window opens, so one whose earliest finish is already too late for
    // what follows cannot be saved by arriving early.
    Seconds latestDeparture = endLimit_ == kNoLimit ? kNoLimit : endLimit_ - homeLeg;
    for (std::size_t i = stops_.size(); i-- > 0;) {
        Stop& stop = stops_[i];
        if (latestDeparture == kNever || stop.minStart + stop.service > latestDeparture) {
            stop.latest = kNever;
        } else {
            stop.latest = std::min(stop.due, latestDeparture - stop.service);
        }
        if (i > 0) {
            latestDeparture =
                    stop.latest == kNever
                            ? kNever
                            : stop.latest - plan.drivingTime(stops_[i - 1].location, stop.location);
        }
    }
}

Seconds RouteTimeline::legTo(const PdpRoutePlan& plan, int location, int position) const {
    if (position < size()) {
        return plan.drivingTime(location, stops_[static_cast<std::size_t>(position)].location);
    }
    return plan.vehicles[static_cast<std::size_t>(vehicle_)].homeLeg(plan.drivingSeconds, location);
}

void RouteTimeline::improve(const PdpRoutePlan& plan, const Delivery& delivery, Blinker& blinker,
                            RequestInsertion& best) const {
    const Vehicle& vehicle = plan.vehicles[static_cast<std::size_t>(vehicle_)];
    const int count = size();
    if (vehicle.maxStops > 0 && count + 2 > vehicle.maxStops) return;
    const std::int64_t demand = delivery.demand;
    const std::int64_t capacity = vehicle.capacity;
    if (demand > capacity) return;

    const Node& pickup = plan.nodes[static_cast<std::size_t>(delivery.pickupNodeIndex)];
    const Node& dropoff = plan.nodes[static_cast<std::size_t>(delivery.dropoffNodeIndex)];
    const Seconds pickupDue = pickup.maxEndTime - pickup.serviceDuration;
    const Seconds dropoffDue = dropoff.maxEndTime - dropoff.serviceDuration;
    const Seconds travelBudget =
            vehicle.maxTravelTime > 0 ? vehicle.maxTravelTime - travel_ : kNoLimit;
    const int firstFree = std::min(static_cast<int>(vehicle.pinnedNodeIndices.size()), count);
    const auto& drive = plan.drivingSeconds;
    const auto pickupLocation = static_cast<std::size_t>(pickup.locationIndex);
    const auto dropoffLocation = static_cast<std::size_t>(dropoff.locationIndex);
    const std::vector<Seconds>& fromDropoff = drive[dropoffLocation];

    for (int i = firstFree; i <= count; ++i) {
        const bool first = i == 0;
        const Stop* previous = first ? nullptr : &stops_[static_cast<std::size_t>(i) - 1];
        const int previousLocation = first ? vehicle.homeLocationIndex : previous->location;
        if ((first ? 0 : previous->load) + demand > capacity) continue;
        const Seconds toPickup = drive[static_cast<std::size_t>(previousLocation)][pickupLocation];
        const Seconds pickupArrival =
                (first ? vehicle.departureTime : previous->departure) + toPickup;
        if (pickupArrival > pickupDue) continue;
        // The leg this insertion breaks: none in an idle route, which drives
        // nothing.
        const Seconds brokenLeg = count == 0 ? 0 : legTo(plan, previousLocation, i);

        // Walk the dropoff slot forward, carrying the schedule of the stops
        // that now sit between the pickup and the dropoff.
        int location = pickup.locationIndex;
        Seconds departure = std::max(pickupArrival, pickup.minStartTime) + pickup.serviceDuration;
        Seconds pickupDetour = 0;  // known once a stop is carried
        for (int j = i;; ++j) {
            // A slot is only skipped in favour of one already in hand.
            if (!(best.found() && blinker.skip())) {
                const Seconds toDropoff =
                        drive[static_cast<std::size_t>(location)][dropoffLocation];
                const Seconds dropoffArrival = departure + toDropoff;
                if (dropoffArrival <= dropoffDue) {
                    const Seconds dropoffDeparture =
                            std::max(dropoffArrival, dropoff.minStartTime) +
                            dropoff.serviceDuration;
                    Seconds onward;
                    bool onTime;
                    if (j < count) {
                        const Stop& next = stops_[static_cast<std::size_t>(j)];
                        onward = fromDropoff[static_cast<std::size_t>(next.location)];
                        onTime = dropoffDeparture + onward <= next.latest;
                    } else {
                        onward = vehicle.homeLeg(drive, dropoff.locationIndex);
                        onTime = dropoffDeparture + onward <= endLimit_;
                    }
                    if (onTime) {
                        const Seconds added = j == i ? toPickup + toDropoff + onward - brokenLeg
                                                     : pickupDetour + toDropoff + onward -
                                                               legTo(plan, location, j);
                        if (added < best.addedTravel && added <= travelBudget) {
                            best.vehicle = vehicle_;
                            best.pickupPosition = i;
                            best.dropoffPosition = j + 1;
                            best.addedTravel = added;
                        }
                    }
                }
            }
            if (j == count) break;
            // The stop at j rides between the pickup and the dropoff: it is
            // reached later, with the request on board.
            const Stop& carried = stops_[static_cast<std::size_t>(j)];
            if (carried.load + demand > capacity) break;
            const Seconds leg = drive[static_cast<std::size_t>(location)]
                                     [static_cast<std::size_t>(carried.location)];
            const Seconds arrival = departure + leg;
            if (arrival > carried.due) break;
            if (j == i) pickupDetour = toPickup + leg - brokenLeg;
            departure = std::max(arrival, carried.minStart) + carried.service;
            location = carried.location;
        }
    }
}

Seconds RouteTimeline::removalGain(const PdpRoutePlan& plan, int pickupPosition,
                                   int dropoffPosition) const {
    if (size() <= 2) return travel_;  // the route empties
    const Vehicle& vehicle = plan.vehicles[static_cast<std::size_t>(vehicle_)];
    const auto before = [&](int position) {
        return position > 0 ? stops_[static_cast<std::size_t>(position) - 1].location
                            : vehicle.homeLocationIndex;
    };
    const int pickup = stops_[static_cast<std::size_t>(pickupPosition)].location;
    const int dropoff = stops_[static_cast<std::size_t>(dropoffPosition)].location;
    const int beforePickup = before(pickupPosition);
    if (dropoffPosition == pickupPosition + 1) {
        return plan.drivingTime(beforePickup, pickup) + plan.drivingTime(pickup, dropoff) +
               legTo(plan, dropoff, dropoffPosition + 1) -
               legTo(plan, beforePickup, dropoffPosition + 1);
    }
    const int beforeDropoff = before(dropoffPosition);
    return plan.drivingTime(beforePickup, pickup) + legTo(plan, pickup, pickupPosition + 1) -
           legTo(plan, beforePickup, pickupPosition + 1) +
           plan.drivingTime(beforeDropoff, dropoff) + legTo(plan, dropoff, dropoffPosition + 1) -
           legTo(plan, beforeDropoff, dropoffPosition + 1);
}

// ---------------------------------------------------------------------------
// PlanTimelines
// ---------------------------------------------------------------------------

void PlanTimelines::rebuild(const PdpRoutePlan& plan) {
    const int vehicleCount = static_cast<int>(plan.vehicles.size());
    timelines_.resize(plan.vehicles.size());
    for (int v = 0; v < vehicleCount; ++v) {
        timelines_[static_cast<std::size_t>(v)].rebuild(
                plan, v, plan.vehicles[static_cast<std::size_t>(v)].nodeIndices);
    }

    // The vehicles do not change during a solve: classify them once (the
    // class contract: one object, one plan).
    if (vehicleClass_.size() == plan.vehicles.size()) return;
    vehicleClass_.assign(plan.vehicles.size(), -1);
    std::vector<int> representatives;
    for (int v = 0; v < vehicleCount; ++v) {
        int& vehicleClass = vehicleClass_[static_cast<std::size_t>(v)];
        for (std::size_t c = 0; c < representatives.size(); ++c) {
            if (sameService(plan, plan.vehicles[static_cast<std::size_t>(representatives[c])],
                            plan.vehicles[static_cast<std::size_t>(v)])) {
                vehicleClass = static_cast<int>(c);
                break;
            }
        }
        if (vehicleClass < 0) {
            vehicleClass = static_cast<int>(representatives.size());
            representatives.push_back(v);
        }
    }
    classCount_ = static_cast<int>(representatives.size());
}

void PlanTimelines::rebuildRoute(const PdpRoutePlan& plan, int vehicleIndex,
                                 const std::vector<int>& route) {
    timelines_[static_cast<std::size_t>(vehicleIndex)].rebuild(plan, vehicleIndex, route);
}

void PlanTimelines::candidateVehicles(const PdpRoutePlan& plan, int requiredVehicle,
                                      std::vector<int>& out) const {
    out.clear();
    if (requiredVehicle >= 0) {
        out.push_back(requiredVehicle);
        return;
    }
    idleClassSeen_.assign(static_cast<std::size_t>(classCount_), 0);
    const int vehicleCount = static_cast<int>(plan.vehicles.size());
    for (int v = 0; v < vehicleCount; ++v) {
        if (timelines_[static_cast<std::size_t>(v)].size() == 0) {
            char& seen = idleClassSeen_[static_cast<std::size_t>(
                    vehicleClass_[static_cast<std::size_t>(v)])];
            if (seen) continue;
            seen = 1;
        }
        out.push_back(v);
    }
}

RequestInsertion PlanTimelines::best(const PdpRoutePlan& plan, const Delivery& delivery,
                                     Blinker& blinker) const {
    RequestInsertion best;
    candidateVehicles(plan, delivery.requiredVehicleIndex, candidateScratch_);
    for (int v : candidateScratch_) {
        timelines_[static_cast<std::size_t>(v)].improve(plan, delivery, blinker, best);
    }
    return best;
}

// ---------------------------------------------------------------------------
// RequestGroups
// ---------------------------------------------------------------------------

void RequestGroups::build(const PdpRoutePlan& plan) {
    const std::size_t count = plan.deliveries.size();
    leader_.resize(count);
    next_.assign(count, -1);
    size_.assign(count, 1);
    vehicle_.resize(count);
    pinned_.assign(count, 0);
    any_ = false;
    std::vector<int> pinnedOn(count, -1);
    for (std::size_t v = 0; v < plan.vehicles.size(); ++v) {
        for (int nodeIndex : plan.vehicles[v].pinnedNodeIndices) {
            const auto d = static_cast<std::size_t>(
                    plan.nodes[static_cast<std::size_t>(nodeIndex)].deliveryIndex);
            pinned_[d] = 1;
            pinnedOn[d] = static_cast<int>(v);
        }
    }
    std::vector<int> grouped;
    for (std::size_t d = 0; d < count; ++d) {
        leader_[d] = static_cast<int>(d);
        vehicle_[d] = plan.deliveries[d].requiredVehicleIndex;
        if (plan.deliveries[d].groupIndex >= 0) grouped.push_back(static_cast<int>(d));
    }
    std::stable_sort(grouped.begin(), grouped.end(), [&plan](int a, int b) {
        return plan.deliveries[static_cast<std::size_t>(a)].groupIndex <
               plan.deliveries[static_cast<std::size_t>(b)].groupIndex;
    });
    for (std::size_t first = 0; first < grouped.size();) {
        const int group = plan.deliveries[static_cast<std::size_t>(grouped[first])].groupIndex;
        std::size_t last = first;
        while (last < grouped.size() &&
               plan.deliveries[static_cast<std::size_t>(grouped[last])].groupIndex == group) {
            ++last;
        }
        // The vehicle the group is bound to: a required one, else the one a
        // pinned stop of it rides.
        int bound = -1;
        for (std::size_t i = first; i < last && bound < 0; ++i) {
            bound = plan.deliveries[static_cast<std::size_t>(grouped[i])].requiredVehicleIndex;
        }
        for (std::size_t i = first; i < last && bound < 0; ++i) {
            bound = pinnedOn[static_cast<std::size_t>(grouped[i])];
        }
        int leader = -1;
        int previous = -1;
        for (std::size_t i = first; i < last; ++i) {
            const auto d = static_cast<std::size_t>(grouped[i]);
            if (vehicle_[d] < 0) vehicle_[d] = bound;
            if (pinned_[d]) continue;  // stays where it is, a unit of its own
            if (leader < 0) {
                leader = grouped[i];
                size_[d] = 0;
            } else {
                next_[static_cast<std::size_t>(previous)] = grouped[i];
            }
            leader_[d] = leader;
            ++size_[static_cast<std::size_t>(leader)];
            previous = grouped[i];
        }
        if (leader >= 0 && size_[static_cast<std::size_t>(leader)] > 1) any_ = true;
        first = last;
    }
}

// ---------------------------------------------------------------------------
// UnitPlanner
// ---------------------------------------------------------------------------

bool UnitPlanner::plan(const PdpRoutePlan& plan, const RouteTimeline& timeline,
                       const std::vector<int>& route, const std::vector<int>& requests,
                       Blinker& blinker, std::vector<RequestInsertion>& steps,
                       Seconds& addedTravel) {
    const std::size_t mark = steps.size();
    const RouteTimeline* current = &timeline;
    Seconds total = 0;
    for (std::size_t r = 0; r < requests.size(); ++r) {
        const Delivery& delivery = plan.deliveries[static_cast<std::size_t>(requests[r])];
        RequestInsertion insertion;
        current->improve(plan, delivery, blinker, insertion);
        if (!insertion.found()) {
            steps.resize(mark);
            return false;
        }
        steps.push_back(insertion);
        total += insertion.addedTravel;
        if (r + 1 == requests.size()) break;
        // The next request sees the route with this one in.
        if (r == 0) route_ = route;
        route_.insert(route_.begin() + insertion.pickupPosition, delivery.pickupNodeIndex);
        route_.insert(route_.begin() + insertion.dropoffPosition, delivery.dropoffNodeIndex);
        timeline_.rebuild(plan, timeline.vehicle(), route_);
        current = &timeline_;
    }
    addedTravel = total;
    return true;
}

}  // namespace pdp

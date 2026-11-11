#include "vrp/visit_insertion.hpp"

#include <algorithm>
#include <cstddef>

namespace vrp {

namespace {

// Far enough from the int64 range that a few subtractions never wrap.
constexpr Seconds kNoLimit = std::numeric_limits<Seconds>::max() / 4;
constexpr Seconds kNever = std::numeric_limits<Seconds>::min() / 4;

}  // namespace

// ---------------------------------------------------------------------------
// RouteTimeline
// ---------------------------------------------------------------------------

void RouteTimeline::rebuild(const VehicleRoutePlan& plan, int vehicleIndex,
                            const std::vector<int>& route) {
    const Vehicle& vehicle = plan.vehicles[static_cast<std::size_t>(vehicleIndex)];
    vehicle_ = vehicleIndex;
    stops_.resize(route.size());
    travel_ = 0;
    demand_ = 0;
    endLimit_ = kNoLimit;
    if (vehicle.maxEndTime > 0) endLimit_ = vehicle.maxEndTime;
    if (vehicle.maxRouteDuration > 0) {
        endLimit_ = std::min(endLimit_, vehicle.departureTime + vehicle.maxRouteDuration);
    }
    if (route.empty()) return;

    Seconds departure = vehicle.departureTime;
    int location = vehicle.homeLocationIndex;
    for (std::size_t i = 0; i < route.size(); ++i) {
        const Visit& visit = plan.visits[static_cast<std::size_t>(route[i])];
        Stop& stop = stops_[i];
        stop.location = visit.locationIndex;
        stop.minStart = visit.minStartTime;
        stop.service = visit.serviceDuration;
        stop.due = visit.maxEndTime - visit.serviceDuration;
        const Seconds leg = plan.drivingTime(location, stop.location);
        travel_ += leg;
        departure = std::max(departure + leg, stop.minStart) + stop.service;
        stop.departure = departure;
        demand_ += visit.demand;
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

void RouteTimeline::improve(const VehicleRoutePlan& plan, const Visit& visit, int first, int last,
                            VisitInsertion& best) const {
    const Vehicle& vehicle = plan.vehicles[static_cast<std::size_t>(vehicle_)];
    const int count = size();
    if (vehicle.maxStops > 0 && count + 1 > vehicle.maxStops) return;
    if (demand_ + visit.demand > vehicle.capacity) return;

    const Seconds due = visit.maxEndTime - visit.serviceDuration;
    const Seconds travelBudget =
            vehicle.maxTravelTime > 0 ? vehicle.maxTravelTime - travel_ : kNoLimit;
    const auto& drive = plan.drivingSeconds;
    const auto here = static_cast<std::size_t>(visit.locationIndex);
    for (int i = std::max(first, 0); i <= std::min(last, count); ++i) {
        const bool front = i == 0;
        const Stop* previous = front ? nullptr : &stops_[static_cast<std::size_t>(i) - 1];
        const int previousLocation = front ? vehicle.homeLocationIndex : previous->location;
        const Seconds toVisit = drive[static_cast<std::size_t>(previousLocation)][here];
        const Seconds arrival = (front ? vehicle.departureTime : previous->departure) + toVisit;
        if (arrival > due) continue;
        const Seconds departure = std::max(arrival, visit.minStartTime) + visit.serviceDuration;

        Seconds onward;
        Seconds brokenLeg;  // the leg this insertion breaks; none in an idle route
        bool onTime;
        if (i < count) {
            const Stop& next = stops_[static_cast<std::size_t>(i)];
            onward = drive[here][static_cast<std::size_t>(next.location)];
            brokenLeg = drive[static_cast<std::size_t>(previousLocation)]
                             [static_cast<std::size_t>(next.location)];
            onTime = departure + onward <= next.latest;
        } else {
            onward = vehicle.homeLeg(drive, visit.locationIndex);
            brokenLeg = count == 0 ? 0 : vehicle.homeLeg(drive, previousLocation);
            onTime = departure + onward <= endLimit_;
        }
        if (!onTime) continue;
        const Seconds added = toVisit + onward - brokenLeg;
        if (added < best.addedTravel && added <= travelBudget) {
            best.vehicle = vehicle_;
            best.position = i;
            best.addedTravel = added;
        }
    }
}

// ---------------------------------------------------------------------------
// VisitGroups
// ---------------------------------------------------------------------------

void VisitGroups::build(const VehicleRoutePlan& plan) {
    const std::size_t count = plan.visits.size();
    leader_.resize(count);
    next_.assign(count, -1);
    size_.assign(count, 1);
    groups_.clear();
    followers_.assign(count, {});
    std::vector<int> grouped;
    for (std::size_t v = 0; v < count; ++v) {
        leader_[v] = static_cast<int>(v);
        if (plan.visits[v].groupIndex >= 0) grouped.push_back(static_cast<int>(v));
        for (int earlier : plan.visits[v].afterVisitIndices) {
            followers_[static_cast<std::size_t>(earlier)].push_back(static_cast<int>(v));
        }
    }
    std::stable_sort(grouped.begin(), grouped.end(), [&plan](int a, int b) {
        return plan.visits[static_cast<std::size_t>(a)].groupIndex <
               plan.visits[static_cast<std::size_t>(b)].groupIndex;
    });

    std::vector<int> waitingFor(count, 0);  // visits it comes after and that are not listed yet
    std::vector<int> ready;
    std::vector<int> order;
    for (std::size_t first = 0; first < grouped.size();) {
        const int group = plan.visits[static_cast<std::size_t>(grouped[first])].groupIndex;
        std::size_t last = first;
        while (last < grouped.size() &&
               plan.visits[static_cast<std::size_t>(grouped[last])].groupIndex == group) {
            ++last;
        }
        if (last - first > 1) {
            // The group in an order that names no visit before one it comes
            // after: the lowest index among the visits whose earlier ones are
            // all listed goes next.
            ready.clear();
            order.clear();
            for (std::size_t i = first; i < last; ++i) {
                const auto v = static_cast<std::size_t>(grouped[i]);
                waitingFor[v] = static_cast<int>(plan.visits[v].afterVisitIndices.size());
                if (waitingFor[v] == 0) ready.push_back(grouped[i]);
            }
            while (!ready.empty()) {
                const auto lowest = std::min_element(ready.begin(), ready.end());
                const int visit = *lowest;
                ready.erase(lowest);
                order.push_back(visit);
                for (int follower : followers_[static_cast<std::size_t>(visit)]) {
                    if (--waitingFor[static_cast<std::size_t>(follower)] == 0)
                        ready.push_back(follower);
                }
            }
            // A cycle (refused before a solve) leaves visits unlisted: list
            // them as they come, so every visit is in one unit whatever the
            // input.
            if (order.size() < last - first) {
                for (std::size_t i = first; i < last; ++i) {
                    if (std::find(order.begin(), order.end(), grouped[i]) == order.end())
                        order.push_back(grouped[i]);
                }
            }
            const int leader = order.front();
            groups_.push_back(leader);
            size_[static_cast<std::size_t>(leader)] = static_cast<int>(order.size());
            for (std::size_t i = 0; i < order.size(); ++i) {
                leader_[static_cast<std::size_t>(order[i])] = leader;
                next_[static_cast<std::size_t>(order[i])] =
                        i + 1 < order.size() ? order[i + 1] : -1;
            }
        }
        first = last;
    }
}

// ---------------------------------------------------------------------------
// UnitPlanner
// ---------------------------------------------------------------------------

bool UnitPlanner::plan(const VehicleRoutePlan& plan, const VisitGroups& groups,
                       const RouteTimeline& timeline, const std::vector<int>& route,
                       const std::vector<int>& visits, std::vector<UnitStep>& steps,
                       Seconds& addedTravel) {
    const std::size_t mark = steps.size();
    if (position_.size() != plan.visits.size()) {
        position_.assign(plan.visits.size(), 0);
        stamp_.assign(plan.visits.size(), 0);
    }
    const RouteTimeline* current = &timeline;
    const std::vector<int>* working = &route;
    Seconds total = 0;
    for (std::size_t r = 0; r < visits.size(); ++r) {
        const Visit& visit = plan.visits[static_cast<std::size_t>(visits[r])];
        // The slots its order leaves it: behind the visits it comes after,
        // in front of the ones that come after it, as far as they are here.
        int first = 0;
        int last = static_cast<int>(working->size());
        const std::vector<int>& followers = groups.followersOf(visits[r]);
        if (!visit.afterVisitIndices.empty() || !followers.empty()) {
            ++epoch_;
            for (std::size_t p = 0; p < working->size(); ++p) {
                const auto at = static_cast<std::size_t>((*working)[p]);
                position_[at] = static_cast<int>(p);
                stamp_[at] = epoch_;
            }
            for (int earlier : visit.afterVisitIndices) {
                const auto e = static_cast<std::size_t>(earlier);
                if (stamp_[e] == epoch_) first = std::max(first, position_[e] + 1);
            }
            for (int later : followers) {
                const auto l = static_cast<std::size_t>(later);
                if (stamp_[l] == epoch_) last = std::min(last, position_[l]);
            }
        }
        VisitInsertion slot;
        if (first <= last) current->improve(plan, visit, first, last, slot);
        if (!slot.found()) {
            steps.resize(mark);
            return false;
        }
        steps.push_back({visits[r], slot});
        total += slot.addedTravel;
        if (r + 1 == visits.size()) break;
        // The next visit sees the route with this one in.
        if (r == 0) route_ = route;
        route_.insert(route_.begin() + slot.position, visits[r]);
        working = &route_;
        timeline_.rebuild(plan, timeline.vehicle(), route_);
        current = &timeline_;
    }
    addedTravel = total;
    return true;
}

}  // namespace vrp

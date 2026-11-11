// Visit groups: the insertion scan against a brute-force enumeration, the
// order a group's visits are listed and placed in, and groups inside a solve
// (visits that ride one vehicle together, some of them before others).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "optimaru/testing/check.hpp"
#include "vrp/constraints.hpp"
#include "vrp/demo_data.hpp"
#include "vrp/domain.hpp"
#include "vrp/group_moves.hpp"
#include "vrp/score_calculator.hpp"
#include "vrp/solver.hpp"
#include "vrp/visit_insertion.hpp"

using namespace vrp;

namespace {

using optimaru::testing::check;
using optimaru::testing::failures;

// The route as the constraints see it, computed the slow way: one walk, every
// limit checked on the way.
struct Walk {
    bool feasible = true;
    Seconds travel = 0;
};

Walk walk(const VehicleRoutePlan& plan, int vehicleIndex, const std::vector<int>& route) {
    const Vehicle& vehicle = plan.vehicles[static_cast<std::size_t>(vehicleIndex)];
    Walk result;
    if (route.empty()) return result;
    std::int64_t demand = 0;
    Seconds departure = vehicle.departureTime;
    int location = vehicle.homeLocationIndex;
    for (int visitIndex : route) {
        const Visit& visit = plan.visits[static_cast<std::size_t>(visitIndex)];
        demand += visit.demand;
        const Seconds leg = plan.drivingTime(location, visit.locationIndex);
        result.travel += leg;
        const Seconds arrival = departure + leg;
        if (arrival + visit.serviceDuration > visit.maxEndTime) result.feasible = false;
        departure = std::max(arrival, visit.minStartTime) + visit.serviceDuration;
        location = visit.locationIndex;
    }
    if (demand > vehicle.capacity) result.feasible = false;
    const Seconds homeLeg = vehicle.homeLeg(plan.drivingSeconds, location);
    result.travel += homeLeg;
    const Seconds end = departure + homeLeg;
    if (vehicle.maxEndTime > 0 && end > vehicle.maxEndTime) result.feasible = false;
    if (vehicle.maxRouteDuration > 0 && end - vehicle.departureTime > vehicle.maxRouteDuration) {
        result.feasible = false;
    }
    if (vehicle.maxTravelTime > 0 && result.travel > vehicle.maxTravelTime) {
        result.feasible = false;
    }
    if (vehicle.maxStops > 0 && static_cast<int>(route.size()) > vehicle.maxStops) {
        result.feasible = false;
    }
    return result;
}

std::vector<int> withVisit(const std::vector<int>& route, int visitIndex, int position) {
    std::vector<int> candidate = route;
    candidate.insert(candidate.begin() + position, visitIndex);
    return candidate;
}

// The cheapest feasible slot among first .. last by trying every one.
VisitInsertion bruteForce(const VehicleRoutePlan& plan, int vehicleIndex,
                          const std::vector<int>& route, int visitIndex, int first, int last) {
    VisitInsertion best;
    const Seconds before = walk(plan, vehicleIndex, route).travel;
    for (int i = std::max(first, 0); i <= std::min(last, static_cast<int>(route.size())); ++i) {
        const Walk candidate = walk(plan, vehicleIndex, withVisit(route, visitIndex, i));
        if (!candidate.feasible) continue;
        const Seconds added = candidate.travel - before;
        if (added < best.addedTravel) best = {vehicleIndex, i, added};
    }
    return best;
}

// Narrower windows and fewer seats than the generator's defaults.
VehicleRoutePlan tighten(VehicleRoutePlan plan, int capacity, Seconds windowMinutes) {
    for (Vehicle& vehicle : plan.vehicles) {
        vehicle.capacity = capacity;
        vehicle.returnToDepot = true;
    }
    for (Visit& visit : plan.visits) visit.maxEndTime = visit.minStartTime + windowMinutes * 60;
    return plan;
}

struct Variant {
    const char* name;
    void (*apply)(VehicleRoutePlan&);
};

void testScanMatchesBruteForce() {
    std::printf("== Insertion scan vs brute force ==\n");
    const Variant variants[] = {
            {"open routes", [](VehicleRoutePlan&) {}},
            {"closed routes", [](VehicleRoutePlan& p) { p.vehicles[0].returnToDepot = true; }},
            {"tight windows and seats",
             [](VehicleRoutePlan& p) {
                 p.vehicles[0].capacity = 9;
                 for (Visit& v : p.visits) v.maxEndTime = v.minStartTime + 100 * 60;
             }},
            {"shift end",
             [](VehicleRoutePlan& p) {
                 p.vehicles[0].returnToDepot = true;
                 p.vehicles[0].maxEndTime = hourMinute(15, 0);
             }},
            {"route duration",
             [](VehicleRoutePlan& p) { p.vehicles[0].maxRouteDuration = 7 * 3600; }},
            {"travel budget",
             [](VehicleRoutePlan& p) {
                 p.vehicles[0].returnToDepot = true;
                 p.vehicles[0].maxTravelTime = 2400;
             }},
            {"stop cap", [](VehicleRoutePlan& p) { p.vehicles[0].maxStops = 7; }},
    };
    for (const Variant& variant : variants) {
        int scans = 0;
        int found = 0;
        bool equal = true;
        for (std::uint64_t seed = 0; seed < 30; ++seed) {
            VehicleRoutePlan plan = buildRandomInstance(14, 1, seed);
            variant.apply(plan);
            // A route of the first visits, each at its cheapest slot.
            std::vector<int> route;
            for (int v = 0; v < 6; ++v) {
                const VisitInsertion slot =
                        bruteForce(plan, 0, route, v, 0, static_cast<int>(route.size()));
                if (slot.found()) route = withVisit(route, v, slot.position);
            }
            RouteTimeline timeline;
            timeline.rebuild(plan, 0, route);
            const int size = static_cast<int>(route.size());
            for (int v = 6; v < 14; ++v) {
                // The whole route, and a window of slots as an order would leave.
                const int bounds[][2] = {{0, size}, {size / 2, size}, {1, size / 2}};
                for (const auto& bound : bounds) {
                    VisitInsertion scanned;
                    timeline.improve(plan, plan.visits[static_cast<std::size_t>(v)], bound[0],
                                     bound[1], scanned);
                    const VisitInsertion slow = bruteForce(plan, 0, route, v, bound[0], bound[1]);
                    ++scans;
                    if (slow.found()) ++found;
                    if (scanned.found() != slow.found()) {
                        equal = false;
                    } else if (scanned.found()) {
                        const Walk placed = walk(plan, 0, withVisit(route, v, scanned.position));
                        equal = equal && scanned.addedTravel == slow.addedTravel &&
                                placed.feasible && scanned.position >= bound[0] &&
                                scanned.position <= bound[1] &&
                                placed.travel - walk(plan, 0, route).travel == scanned.addedTravel;
                    }
                }
            }
        }
        std::printf("    %s: %d scans (%d with a slot, %d without)\n", variant.name, scans, found,
                    scans - found);
        check(equal, std::string(variant.name) + ": the scan finds the cheapest feasible slot");
    }
    std::printf("\n");
}

// The visits in the order their windows open (the generator's are morning or
// afternoon), so that serving them in this order is possible.
std::vector<int> byWindow(const VehicleRoutePlan& plan, std::vector<int> visits) {
    std::stable_sort(visits.begin(), visits.end(), [&plan](int a, int b) {
        return plan.visits[static_cast<std::size_t>(a)].minStartTime <
               plan.visits[static_cast<std::size_t>(b)].minStartTime;
    });
    return visits;
}

// Groups the first `grouped` visits `size` at a time. When `chained`, the
// visits of a group are a chain: each after the one whose window opens before
// its own.
void groupInOrder(VehicleRoutePlan& plan, int grouped, int size, bool chained) {
    for (int first = 0; first < grouped; first += size) {
        std::vector<int> members;
        for (int v = first; v < std::min(first + size, grouped); ++v) {
            plan.visits[static_cast<std::size_t>(v)].groupIndex = first / size;
            members.push_back(v);
        }
        if (!chained) continue;
        members = byWindow(plan, members);
        for (std::size_t i = 1; i < members.size(); ++i) {
            plan.visits[static_cast<std::size_t>(members[i])].afterVisitIndices = {members[i - 1]};
        }
    }
}

// The vehicle and the position of each visit, or -1.
struct Places {
    std::vector<int> vehicle;
    std::vector<int> position;
};

Places placesOf(const VehicleRoutePlan& plan) {
    Places places{std::vector<int>(plan.visits.size(), -1),
                  std::vector<int>(plan.visits.size(), -1)};
    for (std::size_t v = 0; v < plan.vehicles.size(); ++v) {
        const std::vector<int>& route = plan.vehicles[v].visitIndices;
        for (std::size_t p = 0; p < route.size(); ++p) {
            places.vehicle[static_cast<std::size_t>(route[p])] = static_cast<int>(v);
            places.position[static_cast<std::size_t>(route[p])] = static_cast<int>(p);
        }
    }
    return places;
}

// Whether every group is whole: on one vehicle, or waiting together.
bool groupsWhole(const VehicleRoutePlan& plan) {
    const Places places = placesOf(plan);
    std::map<int, int> where;
    for (std::size_t v = 0; v < plan.visits.size(); ++v) {
        const int group = plan.visits[v].groupIndex;
        if (group < 0) continue;
        const auto [at, first] = where.emplace(group, places.vehicle[v]);
        if (!first && at->second != places.vehicle[v]) return false;
    }
    return true;
}

// Whether every visit is served after the ones it has to come after, where
// both ride one vehicle.
bool orderKept(const VehicleRoutePlan& plan) {
    const Places places = placesOf(plan);
    for (std::size_t v = 0; v < plan.visits.size(); ++v) {
        for (int earlier : plan.visits[v].afterVisitIndices) {
            const auto e = static_cast<std::size_t>(earlier);
            if (places.vehicle[v] >= 0 && places.vehicle[v] == places.vehicle[e] &&
                places.position[e] > places.position[v]) {
                return false;
            }
        }
    }
    return true;
}

bool everyVisitOnce(const VehicleRoutePlan& plan) {
    std::vector<int> seen(plan.visits.size(), 0);
    for (const Vehicle& vehicle : plan.vehicles) {
        for (int visitIndex : vehicle.visitIndices) ++seen[static_cast<std::size_t>(visitIndex)];
    }
    for (int visitIndex : plan.unassignedVisitIndices) ++seen[static_cast<std::size_t>(visitIndex)];
    return std::all_of(seen.begin(), seen.end(), [](int count) { return count == 1; });
}

int waitingCount(const VehicleRoutePlan& plan) {
    const Places places = placesOf(plan);
    return static_cast<int>(std::count(places.vehicle.begin(), places.vehicle.end(), -1));
}

bool refuses(VehicleRoutePlan plan) {
    try {
        Solver::Config config;
        config.time_limit = std::chrono::minutes(1);  // safety net
        config.step_limit = 20;
        Solver(config).solve(plan);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void testGroupsAreRead() {
    std::printf("== Visit groups: who travels with whom, and in what order ==\n");
    VehicleRoutePlan plan = buildRandomInstance(8, 2, /*seed=*/2);
    for (int v : {0, 2, 5, 6}) plan.visits[static_cast<std::size_t>(v)].groupIndex = 30;
    plan.visits[1].groupIndex = 9;  // a group of one
    // 6 before 0 and 2; 2 before 5; 0 and 5 unrelated.
    plan.visits[0].afterVisitIndices = {6};
    plan.visits[2].afterVisitIndices = {6};
    plan.visits[5].afterVisitIndices = {2};
    VisitGroups groups;
    groups.build(plan);
    check(groups.any() && groups.groups() == std::vector<int>{6},
          "the group of four is the one unit of several visits, led by the visit nothing precedes");
    std::vector<int> listed;
    for (int v = groups.leaderOf(0); v >= 0; v = groups.nextOf(v)) listed.push_back(v);
    check(listed == std::vector<int>{6, 0, 2, 5} && groups.sizeOf(6) == 4 &&
                  groups.leaderOf(5) == 6,
          "its visits are listed so that none comes before one it has to follow");
    check(groups.leaderOf(1) == 1 && groups.sizeOf(1) == 1 && groups.leaderOf(3) == 3 &&
                  groups.nextOf(3) == -1,
          "a group of one and a visit in no group are units of one");
    check(groups.followersOf(6) == std::vector<int>({0, 2}) &&
                  groups.followersOf(2) == std::vector<int>{5} && groups.followersOf(5).empty(),
          "each visit knows the ones that come after it");

    VehicleRoutePlan free = buildRandomInstance(8, 2, /*seed=*/2);
    VisitGroups none;
    none.build(free);
    check(!none.any() &&
                  makeGroupMoves(GroupMoveSettings{}, std::make_shared<const VisitGroups>(none))
                          .empty(),
          "a plan without groups registers no group operator");

    // What a plan may not say.
    VehicleRoutePlan cycle = plan;
    cycle.visits[6].afterVisitIndices = {5};
    check(refuses(cycle), "an order with a cycle is refused");
    VehicleRoutePlan stranger = plan;
    stranger.visits[0].afterVisitIndices = {3};
    check(refuses(stranger), "a visit cannot come after one outside its group");
    VehicleRoutePlan ungrouped = plan;
    ungrouped.visits[3].afterVisitIndices = {4};
    check(refuses(ungrouped), "nor can a visit in no group come after another");
    VehicleRoutePlan self = plan;
    self.visits[2].afterVisitIndices = {2};
    check(refuses(self), "nor after itself");
    VehicleRoutePlan range = plan;
    range.visits[2].afterVisitIndices = {99};
    check(refuses(range), "and an index past the visits is refused");
    VehicleRoutePlan twice = plan;
    twice.visits[5].afterVisitIndices = {2, 2};
    check(refuses(twice), "nor may a visit name another twice");
    check(!refuses(plan), "the plan itself is accepted");
    std::printf("\n");
}

// The planner's steps, applied one after another, give a route that keeps
// every limit and the order, and drives exactly what the planner said: the
// same plan as placing each visit at its cheapest allowed slot the slow way.
void testUnitPlanner() {
    std::printf("== Visit groups: room for several visits on one route ==\n");
    int planned = 0;
    int refused = 0;
    for (std::uint64_t seed = 0; seed < 120; ++seed) {
        VehicleRoutePlan plan =
                tighten(buildRandomInstance(12, 1, seed), /*capacity=*/14,
                        /*windowMinutes=*/static_cast<Seconds>(150 + 30 * (seed % 4)));
        plan.vehicles[0].returnToDepot = seed % 2 == 0;
        if (seed % 7 == 0) plan.vehicles[0].maxStops = 8;
        // Visits 6..9 are a group: one first, one last, two between in any
        // order (the first and the last by their windows).
        for (int v : {6, 7, 8, 9}) plan.visits[static_cast<std::size_t>(v)].groupIndex = 0;
        const std::vector<int> chain = byWindow(plan, {6, 7, 8, 9});
        plan.visits[static_cast<std::size_t>(chain[1])].afterVisitIndices = {chain[0]};
        plan.visits[static_cast<std::size_t>(chain[2])].afterVisitIndices = {chain[0]};
        plan.visits[static_cast<std::size_t>(chain[3])].afterVisitIndices = {chain[1], chain[2]};
        VisitGroups groups;
        groups.build(plan);

        std::vector<int> route;
        for (int v = 0; v < 5; ++v) {
            const VisitInsertion slot =
                    bruteForce(plan, 0, route, v, 0, static_cast<int>(route.size()));
            if (slot.found()) route = withVisit(route, v, slot.position);
        }
        RouteTimeline timeline;
        timeline.rebuild(plan, 0, route);
        std::vector<int> visits;  // the unit in the order the groups list it
        for (int v = groups.leaderOf(6); v >= 0; v = groups.nextOf(v)) visits.push_back(v);
        std::vector<UnitStep> steps = {UnitStep{}};  // one already there
        Seconds added = 0;
        UnitPlanner planner;
        const bool found = planner.plan(plan, groups, timeline, route, visits, steps, added);

        std::vector<int> expected = route;
        bool fits = true;
        for (int v : visits) {
            int first = 0;
            int last = static_cast<int>(expected.size());
            for (std::size_t p = 0; p < expected.size(); ++p) {
                const Visit& visit = plan.visits[static_cast<std::size_t>(v)];
                if (std::find(visit.afterVisitIndices.begin(), visit.afterVisitIndices.end(),
                              expected[p]) != visit.afterVisitIndices.end()) {
                    first = std::max(first, static_cast<int>(p) + 1);
                }
                const std::vector<int>& followers = groups.followersOf(v);
                if (std::find(followers.begin(), followers.end(), expected[p]) != followers.end())
                    last = std::min(last, static_cast<int>(p));
            }
            const VisitInsertion slot = bruteForce(plan, 0, expected, v, first, last);
            if (!slot.found()) {
                fits = false;
                break;
            }
            expected = withVisit(expected, v, slot.position);
        }

        const std::string label = "seed " + std::to_string(seed);
        if (found != fits) {
            check(false, label + ": the planner and the slow way agree on whether the visits fit");
            continue;
        }
        if (!found) {
            ++refused;
            check(steps.size() == 1, label + ": a unit that does not fit leaves the steps alone");
            continue;
        }
        ++planned;
        std::vector<int> built = route;
        for (std::size_t i = 1; i < steps.size(); ++i) {
            built = withVisit(built, steps[i].visit, steps[i].slot.position);
        }
        VehicleRoutePlan check_plan = plan;
        check_plan.vehicles[0].visitIndices = built;
        const Walk before = walk(plan, 0, route);
        const Walk after = walk(plan, 0, built);
        if (!(steps.size() == 5 && after.feasible && orderKept(check_plan) &&
              after.travel - before.travel == added &&
              after.travel == walk(plan, 0, expected).travel)) {
            check(false, label + ": the planned steps keep every limit and the order, and add "
                                 "what the planner said");
        }
    }
    std::printf("    %d planned, %d refused\n", planned, refused);
    check(planned >= 20 && refused >= 5, "the cases cover units that fit and units that do not");
    std::printf("\n");
}

Solver::Config stepLimited(std::int64_t steps) {
    Solver::Config config;
    config.time_limit = std::chrono::minutes(1);  // safety net: the step limit ends the solve
    config.step_limit = steps;
    return config;
}

// Construction alone (one step of search): groups go in whole, in order.
void testConstruction() {
    std::printf("== Visit groups: construction ==\n");
    {
        VehicleRoutePlan plan = buildRandomInstance(30, 4, /*seed=*/4);
        groupInOrder(plan, /*grouped=*/18, /*size=*/3, /*chained=*/true);
        VisitGroups groups;
        groups.build(plan);
        constructGroups(plan, groups);
        const Places places = placesOf(plan);
        bool placed = true;
        for (int v = 0; v < 18; ++v)
            placed = placed && places.vehicle[static_cast<std::size_t>(v)] >= 0;
        check(placed && groupsWhole(plan) && orderKept(plan),
              "every group is placed whole on one vehicle, in its order");
        bool feasible = true;
        for (std::size_t v = 0; v < plan.vehicles.size(); ++v) {
            feasible = feasible &&
                       walk(plan, static_cast<int>(v), plan.vehicles[v].visitIndices).feasible;
        }
        check(feasible && plan.unassignedVisitIndices.empty(),
              "the routes keep their limits and nothing is parked");
    }
    {
        // Two visits of a group start on vehicle 1, one on vehicle 0, one waits:
        // it joins the two.
        VehicleRoutePlan plan = buildRandomInstance(12, 3, /*seed=*/4);
        for (int v : {0, 1, 2, 3}) plan.visits[static_cast<std::size_t>(v)].groupIndex = 0;
        plan.vehicles[0].visitIndices = {0};
        plan.vehicles[1].visitIndices = {1, 2};
        VisitGroups groups;
        groups.build(plan);
        constructGroups(plan, groups);
        check(placesOf(plan).vehicle[3] == 1,
              "a waiting visit joins the vehicle that carries most of its group");
    }
    {
        // No vehicle takes three visits: the group is parked, whole.
        VehicleRoutePlan plan = buildRandomInstance(9, 2, /*seed=*/4);
        for (Vehicle& vehicle : plan.vehicles) vehicle.maxStops = 2;
        for (int v : {0, 1, 2}) plan.visits[static_cast<std::size_t>(v)].groupIndex = 0;
        VisitGroups groups;
        groups.build(plan);
        constructGroups(plan, groups);
        std::vector<int> parked = plan.unassignedVisitIndices;
        std::sort(parked.begin(), parked.end());
        check(parked == std::vector<int>({0, 1, 2}) && plan.vehicles[0].visitIndices.empty() &&
                      plan.vehicles[1].visitIndices.empty(),
              "a group no vehicle takes whole is parked in the pool, whole");
    }
    {
        // The caller parked one visit of a group: it stays parked, as it does
        // for the engine's construction, and the rest of the group waits with
        // it. A group the caller did not touch is placed.
        VehicleRoutePlan plan = buildRandomInstance(9, 2, /*seed=*/4);
        for (int v : {0, 1, 2}) plan.visits[static_cast<std::size_t>(v)].groupIndex = 0;
        for (int v : {3, 4, 5}) plan.visits[static_cast<std::size_t>(v)].groupIndex = 1;
        plan.unassignedVisitIndices = {1};
        VisitGroups groups;
        groups.build(plan);
        constructGroups(plan, groups);
        std::vector<int> parked = plan.unassignedVisitIndices;
        std::sort(parked.begin(), parked.end());
        const Places places = placesOf(plan);
        check(parked == std::vector<int>({0, 1, 2}),
              "a group with a visit the caller parked waits whole in the pool");
        check(places.vehicle[3] >= 0 && places.vehicle[3] == places.vehicle[4] &&
                      places.vehicle[4] == places.vehicle[5],
              "and the other group is placed");
    }
    std::printf("\n");
}

// Groups under the whole search: the group operator carries a group as one
// unit, and the engine's one-visit moves are priced by visitGroupCost and
// visitOrderCost.
void testGroupsRideTogether() {
    std::printf("== Visit groups in a solve ==\n");
    struct Case {
        const char* name;
        bool chained;
        bool tight;  // some groups have to wait, whole
    };
    const Case cases[] = {
            {"groups", false, false},
            {"groups with an order", true, false},
            {"groups with an order, some left out", true, true},
    };
    for (const Case& c : cases) {
        VehicleRoutePlan plan = c.tight ? tighten(buildRandomInstance(40, 2, /*seed=*/11), 12, 150)
                                        : buildRandomInstance(36, 4, /*seed=*/11);
        groupInOrder(plan, /*grouped=*/24, /*size=*/3, c.chained);
        Solver::Config config = stepLimited(4000);
        config.seed = 11;
        config.score_checks = optimaru::ScoreChecks::Full;
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        bool threw = false;
        try {
            Solver(config).solve(plan, calculator);
        } catch (const optimaru::ScoreMismatchError&) {
            threw = true;
        }
        const std::string label = c.name;
        check(!threw && everyVisitOnce(plan),
              label + ": the score stays exact and every visit appears once");
        check(plan.score.feasible() && groupsWhole(plan) && orderKept(plan),
              label + ": every group rides one vehicle in its order, or waits together (" +
                      plan.score.str() + ")");
        if (c.tight) {
            check(waitingCount(plan) > 0, label + ": some visits wait");
        } else {
            check(waitingCount(plan) == 0, label + ": every visit is routed");
        }
    }
    std::printf("\n");
}

// A start with every group spread over the fleet and its order reversed.
void testScatteredStartIsMended() {
    std::printf("== Visit groups: a scattered start ==\n");
    VehicleRoutePlan plan = buildRandomInstance(24, 4, /*seed=*/6);
    groupInOrder(plan, /*grouped=*/12, /*size=*/3, /*chained=*/true);
    for (int v = 23; v >= 0; --v)
        plan.vehicles[static_cast<std::size_t>(v % 4)].visitIndices.push_back(v);
    check(!groupsWhole(plan), "the start has every group on three vehicles");
    Solver::Config config = stepLimited(6000);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    Solver(config).solve(plan, calculator);
    check(plan.score.feasible() && groupsWhole(plan) && orderKept(plan) && everyVisitOnce(plan),
          "the solve brings every group onto one vehicle, in its order (" + plan.score.str() + ")");
    std::printf("\n");
}

// A group no vehicle can take whole waits whole: its visits are not routed
// one by one.
void testGroupWithNoRoomWaits() {
    std::printf("== Visit groups: no vehicle takes the whole group ==\n");
    VehicleRoutePlan plan = buildRandomInstance(7, 2, /*seed=*/9);
    for (Vehicle& vehicle : plan.vehicles) vehicle.maxStops = 2;
    for (int v : {0, 1, 2}) plan.visits[static_cast<std::size_t>(v)].groupIndex = 0;
    Solver::Config config = stepLimited(5000);
    config.score_checks = optimaru::ScoreChecks::Full;
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    Solver(config).solve(plan, calculator);
    const Places places = placesOf(plan);
    check(plan.score.feasible() && places.vehicle[0] < 0 && places.vehicle[1] < 0 &&
                  places.vehicle[2] < 0,
          "the three visits of the group stay unassigned together (" + plan.score.str() + ")");
    check(waitingCount(plan) == 3 && everyVisitOnce(plan), "and the four other visits are routed");

    // The same through solve(plan), which has no calculator to be told about:
    // a plan with groups is scored with the default assembly there too. On the
    // four core constraints the three visits would be routed apart and look fine.
    VehicleRoutePlan core = buildRandomInstance(7, 2, /*seed=*/9);
    for (Vehicle& vehicle : core.vehicles) vehicle.maxStops = 2;
    for (int v : {0, 1, 2}) core.visits[static_cast<std::size_t>(v)].groupIndex = 0;
    Solver(stepLimited(5000)).solve(core);
    const Places corePlaces = placesOf(core);
    check(core.score == plan.score && corePlaces.vehicle[0] < 0 && corePlaces.vehicle[1] < 0 &&
                  corePlaces.vehicle[2] < 0,
          "solve(plan) scores a grouped plan with the default assembly (" + core.score.str() + ")");
    std::printf("\n");
}

// solve(plan) with parallel starts asked for: a grouped plan goes to the
// default assembly, which is one start, and comes back whole and in order.
void testGroupsWithParallelStartsAskedFor() {
    std::printf("== Visit groups through solve(plan), parallel starts asked for ==\n");
    VehicleRoutePlan plan = buildRandomInstance(24, 3, /*seed=*/13);
    groupInOrder(plan, /*grouped=*/12, /*size=*/3, /*chained=*/true);
    Solver::Config config = stepLimited(3000);
    config.parallel_solver_count = 2;
    Solver(config).solve(plan);
    check(everyVisitOnce(plan) && plan.score.feasible() && waitingCount(plan) == 0 &&
                  groupsWhole(plan) && orderKept(plan),
          "the grouped plan is solved whole and in order (" + plan.score.str() + ")");
    std::printf("\n");
}

// calculateScore() reads a plan the way solve(plan) scores it. The four core
// rules know neither groups nor order, so on them a split group looks fine.
void testCalculateScore() {
    std::printf("== Visit groups: calculateScore ==\n");
    VehicleRoutePlan plan = buildRandomInstance(24, 3, /*seed=*/13);
    groupInOrder(plan, /*grouped=*/12, /*size=*/3, /*chained=*/true);
    Solver(stepLimited(3000)).solve(plan);
    const Score solved = plan.score;
    check(calculateScore(plan) == solved && plan.score == solved,
          "a solved grouped plan scores as the solve left it (" + solved.str() + ")");

    // One visit of a group of three moves to another vehicle.
    const Places places = placesOf(plan);
    int moved = -1;
    for (int v = 0; v < 24 && moved < 0; ++v) {
        if (plan.visits[static_cast<std::size_t>(v)].groupIndex >= 0) moved = v;
    }
    const int from = places.vehicle[static_cast<std::size_t>(moved)];
    auto& source = plan.vehicles[static_cast<std::size_t>(from)].visitIndices;
    source.erase(std::find(source.begin(), source.end(), moved));
    plan.vehicles[static_cast<std::size_t>((from + 1) % 3)].visitIndices.push_back(moved);
    const Score split = calculateScore(plan);
    check(split.hard <= -1000 && !split.feasible() && plan.score == split,
          "a split group is priced and stored in plan.score (" + split.str() + ")");
    VehicleRoutePlan onCoreRules = plan;
    check(ScoreCalculator::score(onCoreRules).hard > split.hard,
          "which the four core constraints leave out (" + onCoreRules.score.str() + ")");

    // Without a group of two or more, it is the core score.
    VehicleRoutePlan free = buildRandomInstance(24, 3, /*seed=*/13);
    Solver(stepLimited(1000)).solve(free);
    VehicleRoutePlan again = free;
    check(calculateScore(free) == ScoreCalculator::score(again),
          "a plan without groups scores on the four core constraints");

    // And it refuses what solve() refuses.
    VehicleRoutePlan stranger = buildRandomInstance(6, 2, /*seed=*/3);
    stranger.visits[0].afterVisitIndices = {1};
    bool refused = false;
    try {
        calculateScore(stranger);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    check(refused, "an order outside a group is refused");

    // The order constraint read without that check (a caller's own
    // calculator): an index that is no visit names nothing, and is not read.
    VehicleRoutePlan stray = buildRandomInstance(6, 2, /*seed=*/3);
    stray.visits[0].groupIndex = stray.visits[1].groupIndex = 0;
    stray.visits[1].afterVisitIndices = {0, 999, -1};
    stray.vehicles[0].visitIndices = {1, 0};
    stray.vehicles[1].visitIndices = {2, 3, 4, 5};
    ConstraintAssembly orderOnly;
    orderOnly.emplace<VisitOrderCost>(1.0);
    DecomposedCalculator calculator;
    std::move(orderOnly).into(calculator);
    calculator.rebuild(stray);
    calculator.score();
    check(calculator.level_scores().hard == -1,
          "visitOrderCost counts the one real pair and skips indices that are no visit");
    std::printf("\n");
}

// What the group operator is for. Step budgets, so the same seed gives the
// same result on every machine: the plans are compared after equally many
// moves.
void testGroupMovesMatter() {
    std::printf("== The group operator vs the engine's moves alone ==\n");
    int wins = 0;
    int losses = 0;
    const int seeds = 5;
    for (int seed = 0; seed < seeds; ++seed) {
        VehicleRoutePlan without = tighten(buildRandomInstance(60, 6, /*seed=*/500 + seed),
                                           /*capacity=*/20, /*windowMinutes=*/210);
        groupInOrder(without, /*grouped=*/36, /*size=*/3, /*chained=*/true);
        VehicleRoutePlan with = without;

        Solver::Config base = stepLimited(20'000);
        base.seed = static_cast<std::uint64_t>(seed);
        Solver::Config withoutConfig = base;
        withoutConfig.group_ruin_recreate = false;
        DecomposedCalculator withoutCalculator;
        addDefaultConstraints(withoutCalculator);
        Solver(withoutConfig).solve(without, withoutCalculator);
        DecomposedCalculator withCalculator;
        addDefaultConstraints(withCalculator);
        Solver(base).solve(with, withCalculator);

        std::printf("    seed %d: without %-32s | with %s\n", seed, without.score.str().c_str(),
                    with.score.str().c_str());
        if (with.score > without.score) {
            ++wins;
        } else if (without.score > with.score) {
            ++losses;
        }
    }
    check(wins > losses, "the group operator reaches better plans in the same number of steps");
    std::printf("\n");
}

}  // namespace

int main() {
    testScanMatchesBruteForce();
    testGroupsAreRead();
    testUnitPlanner();
    testConstruction();
    testGroupsRideTogether();
    testScatteredStartIsMended();
    testGroupWithNoRoomWaits();
    testGroupsWithParallelStartsAskedFor();
    testCalculateScore();
    testGroupMovesMatter();
    std::printf("%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

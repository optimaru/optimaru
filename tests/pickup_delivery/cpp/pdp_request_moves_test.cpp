// Request-level search: the two-slot insertion scan against a brute-force
// enumeration, the request moves (requestRuinRecreate, bundleRelocate,
// bundleExchange) inside a solve, and request groups (requests that ride one
// vehicle together).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "pdp/constraints.hpp"
#include "pdp/demo_data.hpp"
#include "pdp/domain.hpp"
#include "pdp/haversine.hpp"
#include "pdp/request_insertion.hpp"
#include "pdp/score_calculator.hpp"
#include "pdp/solver.hpp"
#include "optimaru/testing/check.hpp"

using namespace pdp;

namespace {

using optimaru::testing::check;
using optimaru::testing::failures;

// The route as the constraints see it, computed the slow way: one walk, every
// limit checked on the way.
struct Walk {
    bool feasible = true;
    Seconds travel = 0;
};

Walk walk(const PdpRoutePlan& plan, int vehicleIndex, const std::vector<int>& route) {
    const Vehicle& vehicle = plan.vehicles[static_cast<std::size_t>(vehicleIndex)];
    Walk result;
    if (route.empty()) return result;
    std::int64_t load = 0;
    Seconds departure = vehicle.departureTime;
    int location = vehicle.homeLocationIndex;
    for (int nodeIndex : route) {
        const Node& node = plan.nodes[static_cast<std::size_t>(nodeIndex)];
        const Delivery& delivery = plan.deliveries[static_cast<std::size_t>(node.deliveryIndex)];
        load += node.pickup ? delivery.demand : -delivery.demand;
        if (load > vehicle.capacity) result.feasible = false;
        const Seconds leg = plan.drivingTime(location, node.locationIndex);
        result.travel += leg;
        const Seconds arrival = departure + leg;
        if (arrival + node.serviceDuration > node.maxEndTime) result.feasible = false;
        departure = std::max(arrival, node.minStartTime) + node.serviceDuration;
        location = node.locationIndex;
    }
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

std::vector<int> withRequest(const std::vector<int>& route, const Delivery& delivery,
                             int pickupPosition, int dropoffPosition) {
    std::vector<int> candidate = route;
    candidate.insert(candidate.begin() + pickupPosition, delivery.pickupNodeIndex);
    candidate.insert(candidate.begin() + dropoffPosition, delivery.dropoffNodeIndex);
    return candidate;
}

// The cheapest feasible slot pair by trying every one.
RequestInsertion bruteForce(const PdpRoutePlan& plan, int vehicleIndex,
                            const std::vector<int>& route, const Delivery& delivery) {
    RequestInsertion best;
    if (delivery.requiredVehicleIndex >= 0 && delivery.requiredVehicleIndex != vehicleIndex) {
        return best;
    }
    const Seconds before = walk(plan, vehicleIndex, route).travel;
    const int size = static_cast<int>(route.size());
    const int firstFree = std::min(
            static_cast<int>(
                    plan.vehicles[static_cast<std::size_t>(vehicleIndex)].pinnedNodeIndices.size()),
            size);
    for (int i = firstFree; i <= size; ++i) {
        for (int j = i + 1; j <= size + 1; ++j) {
            const Walk candidate = walk(plan, vehicleIndex, withRequest(route, delivery, i, j));
            if (!candidate.feasible) continue;
            const Seconds added = candidate.travel - before;
            if (added < best.addedTravel) best = {vehicleIndex, i, j, added};
        }
    }
    return best;
}

// The limits a variant of the random instance sets on its vehicles.
struct Variant {
    const char* name;
    bool returnToDepot;
    Seconds windowMinutes;  // 0 keeps the generator's half-day windows
    Seconds maxEndTime;
    Seconds maxRouteDuration;
    Seconds maxTravelTime;
    int maxStops;
    int capacity;  // 0 keeps the generator's
};

PdpRoutePlan buildVariant(const Variant& variant, std::uint64_t seed) {
    PdpRoutePlan plan = buildRandomInstance(14, 3, seed);
    for (Vehicle& vehicle : plan.vehicles) {
        vehicle.returnToDepot = variant.returnToDepot;
        vehicle.maxEndTime = variant.maxEndTime;
        vehicle.maxRouteDuration = variant.maxRouteDuration;
        vehicle.maxTravelTime = variant.maxTravelTime;
        vehicle.maxStops = variant.maxStops;
        if (variant.capacity > 0) vehicle.capacity = variant.capacity;
    }
    if (variant.windowMinutes > 0) {
        for (Node& node : plan.nodes) {
            node.maxEndTime = node.minStartTime + variant.windowMinutes * 60;
        }
    }
    return plan;
}

void testScanMatchesBruteForce() {
    std::printf("== Insertion scan vs brute force ==\n");
    const Variant variants[] = {
            {"open routes", false, 0, 0, 0, 0, 0, 0},
            {"closed routes", true, 0, 0, 0, 0, 0, 0},
            {"tight windows and seats", true, 100, 0, 0, 0, 0, 5},
            {"shift end", true, 0, hourMinute(12, 0), 0, 0, 0, 0},
            {"route duration", false, 0, 0, 3 * 3600, 0, 0, 0},
            {"travel budget", true, 0, 0, 0, 3000, 0, 0},
            {"stop cap", false, 0, 0, 0, 0, 6, 0},
    };
    for (const Variant& variant : variants) {
        int compared = 0;
        int feasible = 0;
        int infeasible = 0;
        bool agree = true;
        bool gainsAgree = true;
        for (std::uint64_t seed = 1; seed <= 6; ++seed) {
            PdpRoutePlan plan = buildVariant(variant, seed);
            PlanTimelines timelines;
            timelines.rebuild(plan);
            Blinker every;
            for (std::size_t d = 0; d < plan.deliveries.size(); ++d) {
                const Delivery& delivery = plan.deliveries[d];
                for (int v = 0; v < static_cast<int>(plan.vehicles.size()); ++v) {
                    const std::vector<int>& route =
                            plan.vehicles[static_cast<std::size_t>(v)].nodeIndices;
                    RequestInsertion scanned;
                    timelines.of(v).improve(plan, delivery, every, scanned);
                    const RequestInsertion expected = bruteForce(plan, v, route, delivery);
                    ++compared;
                    ++(expected.found() ? feasible : infeasible);
                    if (scanned.found() != expected.found()) {
                        agree = false;
                        continue;
                    }
                    if (!scanned.found()) continue;
                    // Same cost, and the slots it names really cost that.
                    const Walk placed = walk(plan, v,
                                             withRequest(route, delivery, scanned.pickupPosition,
                                                         scanned.dropoffPosition));
                    if (scanned.addedTravel != expected.addedTravel || !placed.feasible ||
                        placed.travel - walk(plan, v, route).travel != scanned.addedTravel) {
                        agree = false;
                    }
                }
                // Route two requests in three, so later scans see busy routes
                // and the pin check below has requests left to place.
                if (d % 3 == 2) continue;
                const RequestInsertion best = timelines.best(plan, delivery, every);
                if (!best.found()) continue;
                std::vector<int>& route =
                        plan.vehicles[static_cast<std::size_t>(best.vehicle)].nodeIndices;
                route = withRequest(route, delivery, best.pickupPosition, best.dropoffPosition);
                timelines.rebuildRoute(plan, best.vehicle, route);
            }

            // What taking a routed request out saves.
            for (int v = 0; v < static_cast<int>(plan.vehicles.size()); ++v) {
                const std::vector<int>& route =
                        plan.vehicles[static_cast<std::size_t>(v)].nodeIndices;
                for (std::size_t p = 0; p < route.size(); ++p) {
                    const Node& node = plan.nodes[static_cast<std::size_t>(route[p])];
                    if (!node.pickup) continue;
                    const int dropoffNode = plan.partnerOf(route[p]);
                    const auto at = std::find(route.begin(), route.end(), dropoffNode);
                    std::vector<int> without = route;
                    without.erase(std::remove_if(
                                          without.begin(), without.end(),
                                          [&](int n) { return n == route[p] || n == dropoffNode; }),
                                  without.end());
                    const Seconds expected =
                            walk(plan, v, route).travel - walk(plan, v, without).travel;
                    if (timelines.of(v).removalGain(plan, static_cast<int>(p),
                                                    static_cast<int>(at - route.begin())) !=
                        expected) {
                        gainsAgree = false;
                    }
                }
            }

            // A pinned prefix closes the slots in front of it.
            for (int v = 0; v < static_cast<int>(plan.vehicles.size()); ++v) {
                Vehicle& vehicle = plan.vehicles[static_cast<std::size_t>(v)];
                const std::size_t pinned = std::min<std::size_t>(2, vehicle.nodeIndices.size());
                vehicle.pinnedNodeIndices.assign(
                        vehicle.nodeIndices.begin(),
                        vehicle.nodeIndices.begin() + static_cast<std::ptrdiff_t>(pinned));
                timelines.rebuildRoute(plan, v, vehicle.nodeIndices);
            }
            for (std::size_t d = 2; d < plan.deliveries.size(); d += 3) {
                for (int v = 0; v < static_cast<int>(plan.vehicles.size()); ++v) {
                    const std::vector<int>& route =
                            plan.vehicles[static_cast<std::size_t>(v)].nodeIndices;
                    RequestInsertion scanned;
                    timelines.of(v).improve(plan, plan.deliveries[d], every, scanned);
                    const RequestInsertion expected =
                            bruteForce(plan, v, route, plan.deliveries[d]);
                    ++compared;
                    if (scanned.found() != expected.found() ||
                        (scanned.found() && scanned.addedTravel != expected.addedTravel)) {
                        agree = false;
                    }
                }
            }
        }
        std::printf("    %s: %d scans (%d with a slot, %d without)\n", variant.name, compared,
                    feasible, infeasible);
        check(agree, std::string(variant.name) + ": the scan finds the brute-force cheapest slot");
        check(gainsAgree,
              std::string(variant.name) + ": removal gain equals the travel difference");
    }
    std::printf("\n");
}

void testCandidateVehicles() {
    std::printf("== Candidate vehicles ==\n");
    PdpRoutePlan plan = buildRandomInstance(4, 1, /*seed=*/3);
    // Three vehicles of one depot: the same home, the same limits.
    plan.vehicles.push_back(plan.vehicles[0]);
    plan.vehicles.push_back(plan.vehicles[0]);
    plan.vehicles[1].id = "2";
    plan.vehicles[2].id = "3";
    PlanTimelines timelines;
    timelines.rebuild(plan);
    std::vector<int> offered;
    timelines.candidateVehicles(plan, plan.deliveries[0].requiredVehicleIndex, offered);
    check(offered == std::vector<int>({0}), "identical idle vehicles are offered once");

    plan.vehicles[0].nodeIndices = {plan.deliveries[1].pickupNodeIndex,
                                    plan.deliveries[1].dropoffNodeIndex};
    timelines.rebuildRoute(plan, 0, plan.vehicles[0].nodeIndices);
    timelines.candidateVehicles(plan, plan.deliveries[0].requiredVehicleIndex, offered);
    check(offered == std::vector<int>({0, 1}), "a busy vehicle and the first idle one are offered");

    plan.deliveries[0].requiredVehicleIndex = 2;
    timelines.candidateVehicles(plan, plan.deliveries[0].requiredVehicleIndex, offered);
    check(offered == std::vector<int>({2}), "a bound request is offered to its vehicle only");
    Blinker every;
    check(timelines.best(plan, plan.deliveries[0], every).vehicle == 2,
          "and its cheapest insertion is on that vehicle");

    // A different shift end is a different vehicle.
    PdpRoutePlan mixed = buildRandomInstance(4, 1, /*seed=*/3);
    mixed.vehicles.push_back(mixed.vehicles[0]);
    mixed.vehicles[1].maxEndTime = hourMinute(20, 0);
    PlanTimelines mixedTimelines;
    mixedTimelines.rebuild(mixed);
    mixedTimelines.candidateVehicles(mixed, mixed.deliveries[0].requiredVehicleIndex, offered);
    check(offered == std::vector<int>({0, 1}),
          "idle vehicles with different limits are both offered");

    // Two vehicles of one depot with a location entry each: the same place and
    // the same driving times make them one class, a different driving time two.
    PdpRoutePlan depot = buildRandomInstance(4, 1, /*seed=*/3);
    depot.vehicles.push_back(depot.vehicles[0]);
    const Location home =
            depot.locations[static_cast<std::size_t>(depot.vehicles[0].homeLocationIndex)];
    depot.vehicles[1].homeLocationIndex = addLocation(depot, home.latitude, home.longitude);
    HaversineDrivingTimeCalculator::initDrivingTimeMatrix(depot);
    PlanTimelines depotTimelines;
    depotTimelines.rebuild(depot);
    depotTimelines.candidateVehicles(depot, depot.deliveries[0].requiredVehicleIndex, offered);
    check(offered == std::vector<int>({0}),
          "two location entries of one depot are one vehicle class");

    const auto secondHome = static_cast<std::size_t>(depot.vehicles[1].homeLocationIndex);
    const auto stop = static_cast<std::size_t>(depot.nodes[0].locationIndex);
    depot.drivingSeconds[secondHome][stop] += 60;
    PlanTimelines slowerTimelines;
    slowerTimelines.rebuild(depot);
    slowerTimelines.candidateVehicles(depot, depot.deliveries[0].requiredVehicleIndex, offered);
    check(offered == std::vector<int>({0, 1}),
          "a different driving time from home makes two classes");
    std::printf("\n");
}

void testBlinker() {
    std::printf("== Blinker ==\n");
    Blinker never;
    bool skipped = false;
    for (int i = 0; i < 1000; ++i) skipped = skipped || never.skip();
    check(!skipped, "the default blinker never skips");

    std::mt19937_64 rng(7);
    Blinker off(0.0, rng);
    for (int i = 0; i < 1000; ++i) skipped = skipped || off.skip();
    check(!skipped, "rate 0 never skips");

    Blinker tenth(0.1, rng);
    int skips = 0;
    const int draws = 100000;
    for (int i = 0; i < draws; ++i) skips += tenth.skip() ? 1 : 0;
    std::printf("    rate 0.1: %d of %d skipped\n", skips, draws);
    check(skips > draws * 9 / 100 && skips < draws * 11 / 100, "rate 0.1 skips about one in ten");

    // The edges of the knob: a rate too small to ever fire, one past 1, and
    // a value that is no rate at all.
    Blinker tiny(1e-30, rng);
    Blinker denormal(std::numeric_limits<double>::denorm_min(), rng);
    Blinker invalid(std::numeric_limits<double>::quiet_NaN(), rng);
    bool fired = false;
    for (int i = 0; i < draws; ++i)
        fired = fired || tiny.skip() || denormal.skip() || invalid.skip();
    check(!fired, "a vanishing rate and a NaN never skip");
    Blinker always(2.0, rng);
    skips = 0;
    for (int i = 0; i < draws; ++i) skips += always.skip() ? 1 : 0;
    check(skips > draws * 99 / 100, "a rate of 1 or more skips nearly every candidate");
    std::printf("\n");
}

// Tightens an instance until requests have to be left out: fewer seats and
// narrower windows than the generator's defaults.
PdpRoutePlan tighten(PdpRoutePlan plan, int capacity, Seconds windowMinutes) {
    for (Vehicle& vehicle : plan.vehicles) {
        vehicle.capacity = capacity;
        vehicle.returnToDepot = true;
    }
    for (Node& node : plan.nodes) node.maxEndTime = node.minStartTime + windowMinutes * 60;
    return plan;
}

bool everyStopOnce(const PdpRoutePlan& plan) {
    std::vector<int> seen(plan.nodes.size(), 0);
    for (const Vehicle& vehicle : plan.vehicles) {
        for (int nodeIndex : vehicle.nodeIndices) ++seen[static_cast<std::size_t>(nodeIndex)];
    }
    for (int nodeIndex : plan.unassignedNodeIndices) ++seen[static_cast<std::size_t>(nodeIndex)];
    return std::all_of(seen.begin(), seen.end(), [](int count) { return count == 1; });
}

// Only the operators under test: no element-granular kick, no older pair move.
Solver::Config requestMovesOnly(bool ruinRecreate, bool bundles) {
    Solver::Config config;
    config.time_limit = std::chrono::minutes(1);  // safety net: the step limit ends the solve
    config.pair_moves = false;
    config.pair_exchange_moves = false;
    config.pair_ruin = false;
    config.ruin_recreate = false;
    config.request_ruin_recreate = ruinRecreate;
    config.bundle_moves = bundles;
    return config;
}

void testMovesKeepTheScoreExact() {
    std::printf("== Request moves under FULL_ASSERT ==\n");
    struct Case {
        const char* name;
        bool ruinRecreate;
        bool bundles;
        bool tight;  // requests wait in the pool, so the pool is written too
    };
    const Case cases[] = {
            {"requestRuinRecreate", true, false, false},
            {"requestRuinRecreate, requests left out", true, false, true},
            {"bundleRelocate + bundleExchange", false, true, false},
            {"all three, requests left out", true, true, true},
    };
    for (const Case& c : cases) {
        for (int assembly = 0; assembly < 2; ++assembly) {
            PdpRoutePlan plan = buildRandomInstance(18, 4, /*seed=*/11);
            if (c.tight) plan = tighten(buildRandomInstance(30, 2, /*seed=*/11), 5, 100);
            Solver::Config config = requestMovesOnly(c.ruinRecreate, c.bundles);
            config.seed = 11;
            config.step_limit = 3000;
            config.score_checks = optimaru::ScoreChecks::Full;
            bool threw = false;
            try {
                if (assembly == 0) {
                    Solver(config).solve(plan);
                } else {
                    DecomposedCalculator calculator;
                    addDefaultConstraints(calculator);
                    Solver(config).solve(plan, calculator);
                }
            } catch (const optimaru::ScoreMismatchError&) {
                threw = true;
            }
            const std::string label =
                    std::string(c.name) + (assembly == 0 ? " (default)" : " (constraint assembly)");
            check(!threw, label + ": incremental score == full recompute at every move");
            check(everyStopOnce(plan), label + ": every stop appears exactly once");
            check(plan.score == ScoreCalculator::score(plan) && plan.score.feasible(),
                  label + ": the result is feasible and scored exactly");
        }
    }
    std::printf("\n");
}

// A request split across two vehicles is hard-infeasible; taking it out and
// putting it back whole is the only way these operators have to fix it.
void testSplitRequestIsMended() {
    std::printf("== A split request ==\n");
    PdpRoutePlan plan = buildRandomInstance(6, 2, /*seed=*/4);
    for (std::size_t d = 0; d < plan.deliveries.size(); ++d) {
        const Delivery& delivery = plan.deliveries[d];
        if (d == 0) {
            plan.vehicles[0].nodeIndices.push_back(delivery.pickupNodeIndex);
            plan.vehicles[1].nodeIndices.push_back(delivery.dropoffNodeIndex);
        } else {
            std::vector<int>& route = plan.vehicles[d % 2].nodeIndices;
            route.push_back(delivery.pickupNodeIndex);
            route.push_back(delivery.dropoffNodeIndex);
        }
    }
    check(ScoreCalculator::score(plan).hard < 0, "the starting plan has a split request");
    Solver::Config config = requestMovesOnly(/*ruinRecreate=*/true, /*bundles=*/true);
    config.pair_construction_heuristic = false;
    config.step_limit = 2000;
    Solver(config).solve(plan);
    check(plan.score.feasible() && everyStopOnce(plan), "the solve puts it back on one vehicle");
    std::printf("\n");
}

// On a large fleet a request is first offered to the routes of its nearby
// stops. Here those are all full and the one route with room is far away: the
// request has to be offered to the rest of the fleet before it is given up.
void testFarRouteIsOfferedWhenNearOnesAreFull() {
    std::printf("== A large fleet: room only on a far route ==\n");
    // Enough vehicles that the offer stays narrowed whatever a move takes out:
    // emptied routes are idle vehicles, which are offered once between them.
    constexpr int kVehicles = 60;
    PdpRoutePlan plan;
    plan.name = "far-route";
    const int home = addLocation(plan, 43.77, 11.25);
    std::mt19937_64 rng(17);
    const auto near = [&](double center) {
        return center + (static_cast<double>(rng() % 2001) - 1000.0) * 1e-5;  // within ~1 km
    };
    for (int v = 0; v < kVehicles; ++v) {
        Vehicle vehicle;
        vehicle.id = std::to_string(v + 1);
        vehicle.capacity = 10;
        vehicle.homeLocationIndex = home;
        vehicle.departureTime = hourMinute(7, 0);
        vehicle.maxStops = 4;  // two requests
        plan.vehicles.push_back(vehicle);
    }
    const auto addRequest = [&](double latitude, double longitude) {
        const int deliveryIndex = static_cast<int>(plan.deliveries.size());
        const int pickupNode = static_cast<int>(plan.nodes.size());
        const std::string number = std::to_string(deliveryIndex + 1);
        plan.nodes.push_back(Node{"p" + number, addLocation(plan, near(latitude), near(longitude)),
                                  hourMinute(7, 0), hourMinute(23, 0), 300, deliveryIndex, true, -1,
                                  NULL_TIME});
        plan.nodes.push_back(Node{"d" + number, addLocation(plan, near(latitude), near(longitude)),
                                  hourMinute(7, 0), hourMinute(23, 0), 300, deliveryIndex, false,
                                  -1, NULL_TIME});
        plan.deliveries.push_back(Delivery{"r" + number, pickupNode, pickupNode + 1, 1});
        return deliveryIndex;
    };
    const auto route = [&](int vehicle, int deliveryIndex) {
        // By value: addRequest grows the vectors these would point into.
        const Delivery delivery = plan.deliveries[static_cast<std::size_t>(deliveryIndex)];
        std::vector<int>& stops = plan.vehicles[static_cast<std::size_t>(vehicle)].nodeIndices;
        stops.push_back(delivery.pickupNodeIndex);
        stops.push_back(delivery.dropoffNodeIndex);
    };
    // Every vehicle but the last is full with two requests in town.
    for (int v = 0; v + 1 < kVehicles; ++v) {
        route(v, addRequest(43.77, 11.25));
        route(v, addRequest(43.77, 11.25));
    }
    // The last one serves a single request an hour's drive away, so none of its
    // stops is near anything in town. Its pickup is pinned: the request stays,
    // so that vehicle is never idle (an idle vehicle is offered to everyone).
    route(kVehicles - 1, addRequest(44.20, 11.90));
    plan.vehicles.back().pinnedNodeIndices = {plan.vehicles.back().nodeIndices.front()};
    // One more request in town, waiting in the pool. Construction would ask
    // every vehicle, so it is off: the search has to route the request.
    addRequest(43.77, 11.25);
    plan.rebuildUnassignedPool();
    HaversineDrivingTimeCalculator::initDrivingTimeMatrix(plan);

    Solver::Config config = requestMovesOnly(/*ruinRecreate=*/true, /*bundles=*/false);
    config.pair_construction_heuristic = false;
    // The engine's own segment moves can carry both stops out of the pool in
    // one step; without them only the request move can route the request.
    config.or_opt_weight = 0.0;
    config.tail_swap_weight = 0.0;
    config.step_limit = 3000;
    config.score_checks = optimaru::ScoreChecks::Full;
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    Solver(config).solve(plan, calculator);
    check(plan.score.feasible() && everyStopOnce(plan), "the plan stays feasible");
    check(plan.unassignedNodeIndices.empty(),
          "the last request is routed although no nearby route has room");
    std::printf("\n");
}

// A start that breaks time windows. Routes that are late take no request in
// front of the late stop, but requests can be taken off them.
void testInfeasibleStartIsMended() {
    std::printf("== An infeasible start ==\n");
    PdpRoutePlan plan = buildRandomInstance(12, 4, /*seed=*/8);
    // Every request on the first vehicle, in input order: the morning requests
    // it reaches in the afternoon are late.
    for (const Delivery& delivery : plan.deliveries) {
        plan.vehicles[0].nodeIndices.push_back(delivery.pickupNodeIndex);
        plan.vehicles[0].nodeIndices.push_back(delivery.dropoffNodeIndex);
    }
    const Score start = ScoreCalculator::score(plan);
    check(start.hard < 0, "the starting plan is late (" + start.str() + ")");

    Solver::Config config = requestMovesOnly(/*ruinRecreate=*/true, /*bundles=*/true);
    config.step_limit = 3000;
    config.score_checks = optimaru::ScoreChecks::Full;
    bool threw = false;
    try {
        Solver(config).solve(plan);
    } catch (const optimaru::ScoreMismatchError&) {
        threw = true;
    }
    check(!threw && everyStopOnce(plan), "the moves keep the score exact from an infeasible plan");
    check(plan.score.feasible(),
          "taking requests off the late route makes the plan feasible (" + plan.score.str() + ")");

    // No stop can be reached in time at all: the request moves have nothing to
    // offer, and the solve still ends with every stop in place and no worse.
    PdpRoutePlan hopeless = buildRandomInstance(6, 2, /*seed=*/8);
    for (Node& node : hopeless.nodes) node.maxEndTime = hourMinute(7, 0);  // before any departure
    for (const Delivery& delivery : hopeless.deliveries) {
        hopeless.vehicles[0].nodeIndices.push_back(delivery.pickupNodeIndex);
        hopeless.vehicles[0].nodeIndices.push_back(delivery.dropoffNodeIndex);
    }
    const Score hopelessStart = ScoreCalculator::score(hopeless);
    Solver(config).solve(hopeless);
    check(everyStopOnce(hopeless) && hopeless.score >= hopelessStart,
          "a plan with no feasible answer is not made worse (" + hopelessStart.str() + " -> " +
                  hopeless.score.str() + ")");
    std::printf("\n");
}

// A request with a pinned stop is not the request moves' to take: its route has
// to keep starting with that stop.
void testPinnedRequestsStay() {
    std::printf("== Pinned stops ==\n");
    PdpRoutePlan plan = buildRandomInstance(12, 2, /*seed=*/21);
    const Delivery& first = plan.deliveries[0];
    const Delivery& second = plan.deliveries[1];
    plan.vehicles[0].nodeIndices = {first.pickupNodeIndex, second.pickupNodeIndex,
                                    first.dropoffNodeIndex, second.dropoffNodeIndex};
    plan.vehicles[0].pinnedNodeIndices = {first.pickupNodeIndex, second.pickupNodeIndex};
    const std::vector<int> pinned = plan.vehicles[0].pinnedNodeIndices;

    Solver::Config config = requestMovesOnly(/*ruinRecreate=*/true, /*bundles=*/true);
    config.step_limit = 5000;
    config.score_checks = optimaru::ScoreChecks::Full;
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    Solver(config).solve(plan, calculator);

    const std::vector<int>& route = plan.vehicles[0].nodeIndices;
    check(route.size() >= 2 && route[0] == pinned[0] && route[1] == pinned[1],
          "the route still starts with its pinned stops");
    check(plan.score.feasible() && everyStopOnce(plan) && plan.unassignedNodeIndices.empty(),
          "and every other request is routed around them");
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// Request groups
// ---------------------------------------------------------------------------

// Groups the requests `size` at a time, in input order.
void groupInOrder(PdpRoutePlan& plan, int size) {
    for (std::size_t d = 0; d < plan.deliveries.size(); ++d) {
        plan.deliveries[d].groupIndex = static_cast<int>(d) / size;
    }
}

// The vehicle each request's pickup rides, or -1.
std::vector<int> vehicleOfRequests(const PdpRoutePlan& plan) {
    std::vector<int> onNode(plan.nodes.size(), -1);
    for (std::size_t v = 0; v < plan.vehicles.size(); ++v) {
        for (int nodeIndex : plan.vehicles[v].nodeIndices)
            onNode[static_cast<std::size_t>(nodeIndex)] = static_cast<int>(v);
    }
    std::vector<int> on;
    for (const Delivery& delivery : plan.deliveries)
        on.push_back(onNode[static_cast<std::size_t>(delivery.pickupNodeIndex)]);
    return on;
}

// Whether every group is whole: on one vehicle, or waiting together.
bool groupsWhole(const PdpRoutePlan& plan) {
    const std::vector<int> on = vehicleOfRequests(plan);
    std::map<int, int> place;
    for (std::size_t d = 0; d < plan.deliveries.size(); ++d) {
        const int group = plan.deliveries[d].groupIndex;
        if (group < 0) continue;
        const auto [at, first] = place.emplace(group, on[d]);
        if (!first && at->second != on[d]) return false;
    }
    return true;
}

// The requests apart from their group, as requestGroupCost counts them.
std::int64_t requestsApart(PdpRoutePlan& plan) {
    ConstraintAssembly assembly;
    assembly.emplace<RequestGroupCost>(1.0);
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    calculator.rebuild(plan);
    calculator.score();
    return -static_cast<std::int64_t>(calculator.level_scores().hard);
}

void testGroupsAreRead() {
    std::printf("== Request groups: who travels with whom ==\n");
    PdpRoutePlan plan = buildRandomInstance(6, 3, /*seed=*/2);
    plan.deliveries[0].groupIndex = 40;
    plan.deliveries[2].groupIndex = 40;
    plan.deliveries[4].groupIndex = 40;
    plan.deliveries[1].groupIndex = 7;  // a group of one
    RequestGroups groups;
    groups.build(plan);
    check(groups.any(), "a group of three is a unit of several requests");
    check(groups.leaderOf(0) == 0 && groups.leaderOf(2) == 0 && groups.leaderOf(4) == 0 &&
                  groups.sizeOf(0) == 3 && groups.nextOf(0) == 2 && groups.nextOf(2) == 4 &&
                  groups.nextOf(4) == -1,
          "its unit lists the three requests from the first one");
    check(groups.leaderOf(1) == 1 && groups.sizeOf(1) == 1 && groups.nextOf(1) == -1 &&
                  groups.leaderOf(3) == 3 && groups.sizeOf(3) == 1,
          "a group of one and a request in no group are units of one");
    check(groups.vehicleOf(0) == -1 && groups.vehicleOf(3) == -1, "nothing is bound to a vehicle");

    // One request of the group requires a vehicle: the whole group is bound.
    plan.deliveries[2].requiredVehicleIndex = 1;
    groups.build(plan);
    check(groups.vehicleOf(0) == 1 && groups.vehicleOf(2) == 1 && groups.vehicleOf(4) == 1 &&
                  groups.vehicleOf(1) == -1,
          "a required vehicle binds every request of the group");

    // A request with a pinned stop stays on its route; the rest of the group
    // follows it there.
    plan.deliveries[2].requiredVehicleIndex = -1;
    plan.vehicles[2].nodeIndices = {plan.deliveries[0].pickupNodeIndex,
                                    plan.deliveries[0].dropoffNodeIndex};
    plan.vehicles[2].pinnedNodeIndices = {plan.deliveries[0].pickupNodeIndex};
    groups.build(plan);
    check(groups.pinned(0) && groups.leaderOf(0) == 0 && groups.sizeOf(0) == 1,
          "a pinned request is a unit of its own");
    check(groups.leaderOf(2) == 2 && groups.leaderOf(4) == 2 && groups.sizeOf(2) == 2 &&
                  groups.vehicleOf(2) == 2 && groups.vehicleOf(4) == 2,
          "and the rest of its group is one unit bound to its vehicle");
    check(groups.peersOf(0) == 2 && groups.peersOf(2) == 2 && groups.peersOf(4) == 2 &&
                  groups.peersOf(1) == 0 && groups.peersOf(3) == 0,
          "every request of the group has two peers, the pinned one included");
    check(groups.groupPinnedOn(2) == 2 && groups.groupPinnedOn(4) == 2 &&
                  groups.groupPinnedOn(3) == -1,
          "and knows the vehicle its pinned peer rides");

    // Two requests of one group cannot be promised to two vehicles.
    PdpRoutePlan clash = buildRandomInstance(4, 2, /*seed=*/2);
    clash.deliveries[0].groupIndex = clash.deliveries[1].groupIndex = 0;
    clash.deliveries[0].requiredVehicleIndex = 0;
    clash.deliveries[1].requiredVehicleIndex = 1;
    bool refused = false;
    try {
        Solver().solve(clash);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    check(refused, "a group whose requests require different vehicles is refused");

    // Nor held on two by pinned stops, or by a pinned stop and a required vehicle.
    const auto refuses = [](PdpRoutePlan plan) {
        try {
            Solver::Config config;
            config.time_limit = std::chrono::minutes(1);  // safety net
            config.step_limit = 50;
            DecomposedCalculator calculator;
            addDefaultConstraints(calculator);
            Solver(config).solve(plan, calculator);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    PdpRoutePlan pins = buildRandomInstance(4, 2, /*seed=*/2);
    pins.deliveries[0].groupIndex = pins.deliveries[1].groupIndex = 0;
    for (int v : {0, 1}) {
        const Delivery& held = pins.deliveries[static_cast<std::size_t>(v)];
        pins.vehicles[static_cast<std::size_t>(v)].nodeIndices = {held.pickupNodeIndex,
                                                                  held.dropoffNodeIndex};
        pins.vehicles[static_cast<std::size_t>(v)].pinnedNodeIndices = {held.pickupNodeIndex};
    }
    pins.rebuildUnassignedPool();
    check(refuses(pins), "a group pinned on two vehicles is refused");
    PdpRoutePlan mixed = pins;
    mixed.vehicles[1].nodeIndices.clear();
    mixed.vehicles[1].pinnedNodeIndices.clear();
    mixed.deliveries[1].requiredVehicleIndex = 1;
    mixed.rebuildUnassignedPool();
    check(refuses(mixed), "a group pinned on one vehicle and required on another is refused");
    mixed.deliveries[1].requiredVehicleIndex = 0;
    check(!refuses(mixed), "pinned and required on the same vehicle is accepted");
    std::printf("\n");
}

// The planner's steps, applied one after another, give a route that keeps
// every limit and drives exactly what the planner said: the same plan as
// placing each request at its cheapest slot pair the slow way.
void testUnitPlanner() {
    std::printf("== Request groups: room for several requests on one route ==\n");
    int planned = 0;
    int refused = 0;
    for (std::uint64_t seed = 0; seed < 120; ++seed) {
        // Tight enough that some units have no room.
        PdpRoutePlan plan = tighten(buildRandomInstance(9, 1, seed), /*capacity=*/5,
                                    /*windowMinutes=*/static_cast<Seconds>(120 + 30 * (seed % 5)));
        plan.vehicles[0].returnToDepot = seed % 2 == 0;
        if (seed % 7 == 0) plan.vehicles[0].maxStops = 10;  // room for five requests
        std::vector<int> route;
        for (int d = 0; d < 3; ++d) {
            const RequestInsertion slot = bruteForce(plan, 0, route, plan.deliveries[d]);
            if (!slot.found()) continue;
            route = withRequest(route, plan.deliveries[d], slot.pickupPosition,
                                slot.dropoffPosition);
        }
        RouteTimeline timeline;
        timeline.rebuild(plan, 0, route);
        const std::vector<int> requests = {3, 4, 5};
        std::vector<UnitStep> steps = {UnitStep{}};  // one already there
        Seconds added = 0;
        Blinker every;
        UnitPlanner planner;
        const bool found = planner.plan(plan, timeline, route, requests, every, steps, added);

        std::vector<int> expected = route;
        bool fits = true;
        for (int d : requests) {
            const RequestInsertion slot = bruteForce(plan, 0, expected, plan.deliveries[d]);
            if (!slot.found()) {
                fits = false;
                break;
            }
            expected = withRequest(expected, plan.deliveries[d], slot.pickupPosition,
                                   slot.dropoffPosition);
        }

        const std::string label = "seed " + std::to_string(seed);
        if (found != fits) {
            check(false,
                  label + ": the planner and the slow way agree on whether the requests fit");
            continue;
        }
        if (!found) {
            ++refused;
            check(steps.size() == 1, label + ": a unit that does not fit leaves the steps alone");
            continue;
        }
        ++planned;
        std::vector<int> built = route;
        std::vector<int> order;
        for (std::size_t i = 1; i < steps.size(); ++i) {
            order.push_back(steps[i].delivery);
            built = withRequest(built, plan.deliveries[static_cast<std::size_t>(steps[i].delivery)],
                                steps[i].slot.pickupPosition, steps[i].slot.dropoffPosition);
        }
        const Walk before = walk(plan, 0, route);
        const Walk after = walk(plan, 0, built);
        if (!(order == requests && after.feasible && after.travel - before.travel == added &&
              after.travel == walk(plan, 0, expected).travel)) {
            check(false,
                  label + ": the planned steps keep every limit and add what the planner said");
        }
    }
    std::printf("    %d planned, %d refused\n", planned, refused);
    check(planned >= 20 && refused >= 5, "the cases cover units that fit and units that do not");
    std::printf("\n");
}

// Groups under the whole search: the request moves carry a group as one unit,
// and the stop-granular moves and the kick are priced by requestGroupCost.
void testGroupsRideTogether() {
    std::printf("== Request groups in a solve ==\n");
    struct Case {
        const char* name;
        bool requestMovesOnly;
        bool tight;  // some groups have to wait, whole
    };
    const Case cases[] = {
            {"every operator", false, false},
            {"request moves only", true, false},
            {"every operator, groups left out", false, true},
            {"request moves only, groups left out", true, true},
    };
    for (const Case& c : cases) {
        PdpRoutePlan plan = c.tight ? tighten(buildRandomInstance(30, 2, /*seed=*/11), 5, 100)
                                    : buildRandomInstance(24, 4, /*seed=*/11);
        groupInOrder(plan, c.tight ? 2 : 3);
        Solver::Config config = c.requestMovesOnly
                                        ? requestMovesOnly(/*ruinRecreate=*/true, /*bundles=*/true)
                                        : Solver::Config{};
        config.time_limit = std::chrono::minutes(1);  // safety net: the step limit ends the solve
        config.seed = 11;
        config.step_limit = 4000;
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
        check(!threw && everyStopOnce(plan),
              label + ": the score stays exact and every stop appears once");
        check(plan.score.feasible() && groupsWhole(plan),
              label + ": every group rides one vehicle or waits together (" + plan.score.str() +
                      ")");
        if (c.tight) {
            check(!plan.unassignedNodeIndices.empty(), label + ": some requests wait");
        } else {
            check(plan.unassignedNodeIndices.empty(), label + ": every request is routed");
        }
    }
    std::printf("\n");
}

// A start with a group spread over the fleet: taking the group out and putting
// it back on one vehicle is how it is mended.
void testScatteredGroupIsMended() {
    std::printf("== Request groups: a scattered start ==\n");
    PdpRoutePlan plan = buildRandomInstance(12, 4, /*seed=*/6);
    groupInOrder(plan, 3);
    for (std::size_t d = 0; d < plan.deliveries.size(); ++d) {
        std::vector<int>& route = plan.vehicles[d % 4].nodeIndices;  // one group, three vehicles
        route.push_back(plan.deliveries[d].pickupNodeIndex);
        route.push_back(plan.deliveries[d].dropoffNodeIndex);
    }
    plan.rebuildUnassignedPool();
    check(!groupsWhole(plan) && requestsApart(plan) == 8,
          "the start has every group on three vehicles: two requests apart in each of four");

    for (int requestMoves = 0; requestMoves < 2; ++requestMoves) {
        PdpRoutePlan work = plan;
        Solver::Config config = requestMoves == 1 ? requestMovesOnly(true, true) : Solver::Config{};
        if (requestMoves == 0) {
            // The stop-granular search alone: one request at a time has to
            // find its way to the others.
            config.request_ruin_recreate = false;
            config.bundle_moves = false;
        }
        config.time_limit = std::chrono::minutes(1);
        config.pair_construction_heuristic = false;
        config.step_limit = 20'000;
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        Solver(config).solve(work, calculator);
        check(work.score.feasible() && groupsWhole(work) && everyStopOnce(work),
              std::string(requestMoves == 1 ? "the request moves" : "the stop-granular search") +
                      " bring every group onto one vehicle (" + work.score.str() + ")");
    }
    std::printf("\n");
}

// What binds a group to a vehicle: a required vehicle on one of its requests,
// a pinned stop, or a request of it that already rides a route.
void testGroupsFollowTheirVehicle() {
    std::printf("== Request groups bound to a vehicle ==\n");
    const auto solved = [](PdpRoutePlan plan) {
        Solver::Config config;
        config.time_limit = std::chrono::minutes(1);
        config.step_limit = 3000;
        config.score_checks = optimaru::ScoreChecks::Full;
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        Solver(config).solve(plan, calculator);
        return plan;
    };

    {
        PdpRoutePlan plan = buildRandomInstance(9, 3, /*seed=*/5);
        for (int d : {0, 1, 2}) plan.deliveries[static_cast<std::size_t>(d)].groupIndex = 0;
        plan.deliveries[1].requiredVehicleIndex = 2;
        const PdpRoutePlan result = solved(plan);
        const std::vector<int> on = vehicleOfRequests(result);
        check(result.score.feasible() && on[0] == 2 && on[1] == 2 && on[2] == 2,
              "a required vehicle on one request takes its whole group there");
    }
    {
        PdpRoutePlan plan = buildRandomInstance(9, 3, /*seed=*/5);
        for (int d : {0, 1, 2}) plan.deliveries[static_cast<std::size_t>(d)].groupIndex = 0;
        const Delivery& first = plan.deliveries[0];
        plan.vehicles[1].nodeIndices = {first.pickupNodeIndex, first.dropoffNodeIndex};
        plan.vehicles[1].pinnedNodeIndices = {first.pickupNodeIndex};
        plan.rebuildUnassignedPool();
        const PdpRoutePlan result = solved(plan);
        const std::vector<int> on = vehicleOfRequests(result);
        check(result.score.feasible() && on[0] == 1 && on[1] == 1 && on[2] == 1 &&
                      result.vehicles[1].nodeIndices.front() == first.pickupNodeIndex,
              "a pinned stop keeps its group on its vehicle");
    }
    {
        // Construction alone: the two waiting requests join the one that
        // already rides, though another vehicle is as good for them.
        PdpRoutePlan plan = buildRandomInstance(9, 3, /*seed=*/5);
        for (int d : {0, 1, 2}) plan.deliveries[static_cast<std::size_t>(d)].groupIndex = 0;
        const Delivery& first = plan.deliveries[0];
        plan.vehicles[2].nodeIndices = {first.pickupNodeIndex, first.dropoffNodeIndex};
        plan.rebuildUnassignedPool();
        Solver::Config config;
        config.time_limit = std::chrono::minutes(1);
        config.step_limit = 1;
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        Solver(config).solve(plan, calculator);
        check(plan.score.feasible() && groupsWhole(plan),
              "construction puts waiting requests where their group already rides");
    }
    std::printf("\n");
}

// One request of a group rides while the rest of the group cannot: the pair
// move may drop it, so the group waits whole. (The request moves do this as a
// unit; here they are off.)
void testStrayRequestRejoinsItsWaitingGroup() {
    std::printf("== Request groups: one request riding, the rest waiting ==\n");
    PdpRoutePlan plan = buildRandomInstance(4, 2, /*seed=*/3);
    plan.deliveries[0].groupIndex = plan.deliveries[1].groupIndex = 0;
    // Request 2 of the group can ride nowhere: its pickup closes before any vehicle leaves.
    plan.nodes[static_cast<std::size_t>(plan.deliveries[1].pickupNodeIndex)].maxEndTime =
            hourMinute(7, 0);
    const auto routed = [&plan](int vehicle, int deliveryIndex) {
        const Delivery& delivery = plan.deliveries[static_cast<std::size_t>(deliveryIndex)];
        std::vector<int>& route = plan.vehicles[static_cast<std::size_t>(vehicle)].nodeIndices;
        route.push_back(delivery.pickupNodeIndex);
        route.push_back(delivery.dropoffNodeIndex);
    };
    routed(0, 0);
    routed(0, 2);
    routed(1, 3);
    plan.rebuildUnassignedPool();
    check(requestsApart(plan) == 1, "the start has one request of the group riding alone");

    Solver::Config config;
    config.time_limit = std::chrono::minutes(1);  // safety net: the step limit ends the solve
    config.step_limit = 4000;
    config.pair_construction_heuristic = false;
    config.request_ruin_recreate = false;
    config.bundle_moves = false;
    config.pair_exchange_moves = false;
    config.pair_ruin = false;
    config.ruin_recreate = false;
    // Only the pair move can take both stops of a request to the pool at once.
    config.or_opt_weight = 0.0;
    config.tail_swap_weight = 0.0;
    config.pair_assignment_probability = 0.5;
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    Solver(config).solve(plan, calculator);
    const std::vector<int> on = vehicleOfRequests(plan);
    check(requestsApart(plan) == 0 && on[0] < 0 && on[1] < 0 && everyStopOnce(plan),
          "the pair move drops it, and the group waits whole (" + plan.score.str() + ")");
    std::printf("\n");
}

// A group bound to a vehicle by a required vehicle, all of it waiting: the pair
// move does not take one request onto that vehicle alone. The group rule is
// left out of the score here, so nothing would refuse such a move: if the pair
// move made it, the request would ride.
void testPairMoveDoesNotRouteABoundGroupInParts() {
    std::printf("== Request groups: the pair move and a waiting bound group ==\n");
    PdpRoutePlan plan = buildRandomInstance(6, 2, /*seed=*/7);
    for (int d : {0, 1, 2}) plan.deliveries[static_cast<std::size_t>(d)].groupIndex = 0;
    plan.deliveries[0].requiredVehicleIndex = 1;
    for (int d : {3, 4, 5}) {
        const Delivery& delivery = plan.deliveries[static_cast<std::size_t>(d)];
        plan.vehicles[0].nodeIndices.push_back(delivery.pickupNodeIndex);
        plan.vehicles[0].nodeIndices.push_back(delivery.dropoffNodeIndex);
    }
    plan.rebuildUnassignedPool();

    Solver::Config config;
    config.time_limit = std::chrono::minutes(1);  // safety net: the step limit ends the solve
    config.step_limit = 3000;
    config.pair_construction_heuristic = false;
    config.request_ruin_recreate = false;
    config.bundle_moves = false;
    config.pair_exchange_moves = false;
    config.pair_ruin = false;
    config.ruin_recreate = false;
    // No engine move that carries a stop out of the pool: only the pair move could.
    config.relocate_weight = 0.0;
    config.list_swap_weight = 0.0;
    config.or_opt_weight = 0.0;
    config.tail_swap_weight = 0.0;
    DecomposedCalculator calculator;
    defaultConstraints().drop("requestGroupCost").into(calculator);
    Solver(config).solve(plan, calculator);
    const std::vector<int> on = vehicleOfRequests(plan);
    check(on[0] < 0 && on[1] < 0 && on[2] < 0 && everyStopOnce(plan),
          "the group still waits whole, though no rule in the score asked for it (" +
                  plan.score.str() + ")");
    std::printf("\n");
}

// Parallel starts each build their own groups and operators on their copy of
// the plan.
// Parallel starts on the constraint assembly: every start gets a calculator of
// its own, start i searches with seed + i, and the best plan wins. Step
// limits, so each start is the same search wherever it runs.
void testParallelStartsOnTheAssembly() {
    std::printf("== Request groups: parallel starts, a calculator each ==\n");
    const auto grouped = [] {
        PdpRoutePlan plan = buildRandomInstance(18, 3, /*seed=*/13);
        groupInOrder(plan, 3);
        return plan;
    };
    const auto defaults = [] {
        auto calculator = std::make_unique<DecomposedCalculator>();
        addDefaultConstraints(*calculator);
        return calculator;
    };
    const auto stepLimited = [](std::uint64_t seed, int starts) {
        Solver::Config config;
        config.time_limit = std::chrono::minutes(1);  // safety net: the step limit ends the solve
        config.step_limit = 3000;
        config.seed = seed;
        config.parallel_solver_count = starts;
        return config;
    };
    // What each seed reaches alone, through solve(plan) and the default set.
    std::vector<Score> alone;
    for (std::uint64_t seed = 5; seed < 8; ++seed) {
        PdpRoutePlan plan = grouped();
        Solver(stepLimited(seed, 1)).solve(plan);
        alone.push_back(plan.score);
    }
    const Score best = *std::max_element(alone.begin(), alone.end());

    // solve(plan) with three starts asked for: the best of the three.
    PdpRoutePlan plan = grouped();
    Solver(stepLimited(5, 3)).solve(plan);
    check(plan.score == best && everyStopOnce(plan) && groupsWhole(plan),
          "solve(plan) on a grouped plan returns the best of its three starts (" +
                  plan.score.str() + ")");

    // The factory overload: one calculator per start, made on the calling
    // thread before any start runs.
    PdpRoutePlan made = grouped();
    int calls = 0;
    bool onCaller = true;
    const std::thread::id caller = std::this_thread::get_id();
    Solver(stepLimited(5, 3)).solve(made, [&] {
        ++calls;
        onCaller = onCaller && std::this_thread::get_id() == caller;
        return defaults();
    });
    check(calls == 3 && onCaller, "the factory makes three calculators, on the calling thread");
    check(made.score == best, "and the starts give the same best plan (" + made.score.str() + ")");

    // One start: the single-calculator solve.
    PdpRoutePlan single = grouped();
    Solver(stepLimited(5, 1)).solve(single, defaults);
    check(single.score == alone.front(), "with one start it is that start's solve");

    bool refused = false;
    try {
        PdpRoutePlan none = grouped();
        Solver(stepLimited(5, 1)).solve(none, [] {
            return std::unique_ptr<DecomposedCalculator>();
        });
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    check(refused, "a factory that makes nothing is refused");
    std::printf("\n");
}

// A group no vehicle can take whole waits whole: its requests are not routed
// one by one.
void testGroupWithNoRoomWaits() {
    std::printf("== Request groups: no vehicle takes the whole group ==\n");
    PdpRoutePlan plan = buildRandomInstance(8, 3, /*seed=*/9);
    for (Vehicle& vehicle : plan.vehicles) vehicle.maxStops = 4;  // two requests each
    for (int d : {0, 1, 2}) plan.deliveries[static_cast<std::size_t>(d)].groupIndex = 0;
    Solver::Config config;
    config.time_limit = std::chrono::minutes(1);
    config.step_limit = 6000;
    config.score_checks = optimaru::ScoreChecks::Full;
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    Solver(config).solve(plan, calculator);
    const std::vector<int> on = vehicleOfRequests(plan);
    check(plan.score.feasible() && on[0] < 0 && on[1] < 0 && on[2] < 0,
          "the three requests of the group stay unassigned together (" + plan.score.str() + ")");
    check(plan.unassignedNodeIndices.size() == 6 && everyStopOnce(plan),
          "and the five other requests are routed");

    // The same through solve(plan), which has no calculator to be told about:
    // a plan with groups is scored with the default assembly there too. On the
    // seven core rules the three requests would be routed apart and look fine.
    PdpRoutePlan core = buildRandomInstance(8, 3, /*seed=*/9);
    for (Vehicle& vehicle : core.vehicles) vehicle.maxStops = 4;
    for (int d : {0, 1, 2}) core.deliveries[static_cast<std::size_t>(d)].groupIndex = 0;
    // Without the score checks, which only verify and never steer the search:
    // the same seed and step budget give the same plan.
    config.score_checks = optimaru::ScoreChecks::Off;
    Solver(config).solve(core);
    const std::vector<int> coreOn = vehicleOfRequests(core);
    check(core.score == plan.score && coreOn[0] < 0 && coreOn[1] < 0 && coreOn[2] < 0,
          "solve(plan) scores a grouped plan with the default assembly (" + core.score.str() + ")");
    std::printf("\n");
}

// calculateScore() reads a plan the way solve(plan) scores it. The seven core
// rules do not know groups, so on them a split group looks fine.
void testCalculateScore() {
    std::printf("== Request groups: calculateScore ==\n");
    PdpRoutePlan plan = buildRandomInstance(18, 3, /*seed=*/13);
    groupInOrder(plan, 3);
    Solver::Config config;
    config.time_limit = std::chrono::minutes(1);
    config.step_limit = 3000;
    Solver(config).solve(plan);
    const Score solved = plan.score;
    check(calculateScore(plan) == solved && plan.score == solved,
          "a solved grouped plan scores as the solve left it (" + solved.str() + ")");

    // One request of a group moves to the end of another vehicle's route.
    const std::vector<int> on = vehicleOfRequests(plan);
    check(on[0] >= 0, "the first request rides after the solve");
    const Delivery& moved = plan.deliveries[0];
    auto& source = plan.vehicles[static_cast<std::size_t>(on[0])].nodeIndices;
    source.erase(std::find(source.begin(), source.end(), moved.pickupNodeIndex));
    source.erase(std::find(source.begin(), source.end(), moved.dropoffNodeIndex));
    auto& target = plan.vehicles[static_cast<std::size_t>((on[0] + 1) % 3)].nodeIndices;
    target.push_back(moved.pickupNodeIndex);
    target.push_back(moved.dropoffNodeIndex);
    const Score split = calculateScore(plan);
    check(split.hard <= -1000 && !split.feasible() && plan.score == split,
          "a split group is priced and stored in plan.score (" + split.str() + ")");
    PdpRoutePlan onCoreRules = plan;
    check(ScoreCalculator::score(onCoreRules).hard > split.hard,
          "which the seven core rules leave out (" + onCoreRules.score.str() + ")");

    // Without a group of two or more, it is the core score: a group of one
    // changes nothing.
    PdpRoutePlan free = buildRandomInstance(18, 3, /*seed=*/13);
    free.deliveries[0].groupIndex = 0;
    config.step_limit = 1000;
    Solver(config).solve(free);
    PdpRoutePlan again = free;
    check(!free.hasRequestGroups() && calculateScore(free) == ScoreCalculator::score(again),
          "a plan without groups scores on the seven core rules");
    std::printf("\n");
}

// What the request moves are for. Step budgets, so the same seed gives the
// same result on every machine: the plans are compared after equally many
// moves, of which a request move is the far more expensive one.
void testRequestMovesMatter() {
    std::printf("== Request moves vs the stop-granular search ==\n");
    int wins = 0;
    int losses = 0;
    const int seeds = 5;
    for (int seed = 0; seed < seeds; ++seed) {
        PdpRoutePlan without = tighten(buildRandomInstance(40, 6, /*seed=*/500 + seed),
                                       /*capacity=*/8, /*windowMinutes=*/150);
        PdpRoutePlan with = without;

        Solver::Config base;
        base.step_limit = 30'000;
        base.time_limit = std::chrono::minutes(1);  // safety net only
        base.seed = static_cast<std::uint64_t>(seed);

        Solver::Config withoutConfig = base;
        withoutConfig.request_ruin_recreate = false;
        withoutConfig.bundle_moves = false;
        Solver(withoutConfig).solve(without);
        Solver(base).solve(with);

        std::printf("    seed %d: without %-30s | with %s\n", seed, without.score.str().c_str(),
                    with.score.str().c_str());
        if (with.score > without.score) {
            ++wins;
        } else if (without.score > with.score) {
            ++losses;
        }
    }
    check(wins > losses, "the request moves reach better plans in the same number of steps");
    std::printf("\n");
}

}  // namespace

int main() {
    testScanMatchesBruteForce();
    testCandidateVehicles();
    testBlinker();
    testMovesKeepTheScoreExact();
    testSplitRequestIsMended();
    testFarRouteIsOfferedWhenNearOnesAreFull();
    testInfeasibleStartIsMended();
    testPinnedRequestsStay();
    testGroupsAreRead();
    testUnitPlanner();
    testGroupsRideTogether();
    testScatteredGroupIsMended();
    testGroupsFollowTheirVehicle();
    testStrayRequestRejoinsItsWaitingGroup();
    testPairMoveDoesNotRouteABoundGroupInParts();
    testParallelStartsOnTheAssembly();
    testGroupWithNoRoomWaits();
    testCalculateScore();
    testRequestMovesMatter();
    std::printf("%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

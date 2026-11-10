// Request-level search: the two-slot insertion scan against a brute-force
// enumeration, and the request moves (requestRuinRecreate, bundleRelocate,
// bundleExchange) inside a solve.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "pdp/constraints.hpp"
#include "pdp/demo_data.hpp"
#include "pdp/domain.hpp"
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
    timelines.candidateVehicles(plan, plan.deliveries[0], offered);
    check(offered == std::vector<int>({0}), "identical idle vehicles are offered once");

    plan.vehicles[0].nodeIndices = {plan.deliveries[1].pickupNodeIndex,
                                    plan.deliveries[1].dropoffNodeIndex};
    timelines.rebuildRoute(plan, 0, plan.vehicles[0].nodeIndices);
    timelines.candidateVehicles(plan, plan.deliveries[0], offered);
    check(offered == std::vector<int>({0, 1}), "a busy vehicle and the first idle one are offered");

    plan.deliveries[0].requiredVehicleIndex = 2;
    timelines.candidateVehicles(plan, plan.deliveries[0], offered);
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
    mixedTimelines.candidateVehicles(mixed, mixed.deliveries[0], offered);
    check(offered == std::vector<int>({0, 1}),
          "idle vehicles with different limits are both offered");
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
    testPinnedRequestsStay();
    testRequestMovesMatter();
    std::printf("%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

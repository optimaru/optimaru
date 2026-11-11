// Plugin-assembly (pdp/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedObjective
//       reproduces the monolithic score bit-for-bit on 300 random route
//       layouts (unassigned visits included)
//   (b) incremental score == full recompute across 1500 random sequence moves
//       driven through the engine's Editor protocol (plugin path)
//   (c) partial assemblies: each component isolates its own term
//   (d) weight / level overrides
//   (e) constraint_totals() explains exactly the reported score
//   (f) a 2-second solve(plan, calculator) under ScoreChecks::Full
//   (g) the pair move takes a misplaced bound request to its vehicle
//   (h) pinned stops and bound requests hold when other hard constraints are broken
//   (i) every component's raw cost by hand: one positive and one zero case
//   (j) the catalog (info()) == each component's kDefaultWeight / kDefaultLevel
//   (k) the medium level: fold, hard() / soft() / weight on
//       unassignedRequestsCost, and incremental == full with those overrides
//   (l) repeated components and the offset / clip knobs: a hand-calculated
//       repeat (travel + 0.5 x travel^2), threshold / literal / negative
//       offsets by hand (an idle vehicle counts in literal mode, the pool
//       only for the two components that price it), the global's total
//       shaped once, the literal formula refused on hard at reset and kept
//       on soft and medium, incremental == a fresh recompute over a random
//       walk, every preview == the applied score under ScoreChecks::Full and a
//       ScoreChecks::Full solve with repeated, shifted components
//   (m) negative thresholds (a fixed charge per non-zero unit, real-valued
//       radius included): previews == applied score under ScoreChecks::Full, a
//       ScoreChecks::Full solve, and after a 20000-step solve plan.score == a fresh
//       recompute == the last (incremental) on_new_best report
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "pdp/constraints.hpp"
#include "pdp/demo_data.hpp"
#include "pdp/domain.hpp"
#include "pdp/score_calculator.hpp"
#include "pdp/solver.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/testing/preview_parity.hpp"
#include "optimaru/testing/check.hpp"

using namespace pdp;

namespace {

using optimaru::testing::check;
using optimaru::testing::failures;

Score scoreWith(DecomposedCalculator& calculator, PdpRoutePlan& plan) {
    calculator.rebuild(plan);
    return fromEngineScore(calculator.score());
}

// Spreads every visit over the vehicles at random; `poolRate` of them go to
// the unassigned pool list and another `poolRate` into no list at all.
void randomizeRoutes(PdpRoutePlan& plan, std::mt19937_64& rng, double poolRate) {
    for (Vehicle& vehicle : plan.vehicles) vehicle.nodeIndices.clear();
    plan.unassignedNodeIndices.clear();
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int v = 0; v < static_cast<int>(plan.nodes.size()); ++v) {
        const double roll = unit(rng);
        if (roll < poolRate) {
            plan.unassignedNodeIndices.push_back(v);
            continue;
        }
        if (roll < 2 * poolRate) continue;  // in no list at all
        plan.vehicles[rng() % plan.vehicles.size()].nodeIndices.push_back(v);
    }
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity() {
    std::printf("== (a) Default assembly == monolithic score ==\n");
    PdpRoutePlan plan = buildRandomInstance(20, 4, /*seed=*/11);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    int mismatches = 0;
    const int rounds = 300;
    for (int round = 0; round < rounds; ++round) {
        randomizeRoutes(plan, rng, /*poolRate=*/0.10);
        const Score plugin = scoreWith(calculator, plan);
        const Score monolithic = ScoreCalculator::score(plan);
        if (!(plugin == monolithic)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n", round,
                            plugin.str().c_str(), monolithic.str().c_str());
            }
        }
    }
    check(mismatches == 0, std::to_string(rounds) + " random route layouts score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of sequence
// mutations, driven through the Editor.
// -------------------------------------------------------------------------
void verifyIncremental() {
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    PdpRoutePlan plan = buildRandomInstance(20, 4, /*seed=*/11);
    plan.rebuildUnassignedPool();

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::Editor<PdpRoutePlan> editor(plan, calculator);

    using Descriptor = optimaru::SolutionTraits<PdpRoutePlan>;
    std::mt19937_64 rng(3);
    int mismatches = 0;
    const int steps = 1500;
    for (int step = 0; step < steps; ++step) {
        const int list_count = Descriptor::list_count(plan);
        const int from = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
        if (Descriptor::list_size(plan, from) == 0) continue;
        const int position = static_cast<int>(
                rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, from)));
        const int element = Descriptor::element_at(plan, from, position);
        editor.remove(from, position);

        const int to = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
        const int insert_at = static_cast<int>(
                rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, to) + 1));
        editor.insert(to, insert_at, element);

        const Score incremental = fromEngineScore(editor.score());
        const Score full = ScoreCalculator::score(plan);
        if (!(incremental == full)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n", step,
                            incremental.str().c_str(), full.str().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps) + " random list moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Each component in isolation carries exactly its own level.
// -------------------------------------------------------------------------
void verifyPartialAssemblies() {
    std::printf("== (c) Single-component assemblies ==\n");
    PdpRoutePlan plan = buildRandomInstance(16, 3, /*seed=*/21);
    std::mt19937_64 rng(5);
    randomizeRoutes(plan, rng, /*poolRate=*/0.15);
    const Score full = ScoreCalculator::score(plan);

    {
        DecomposedCalculator calculator;
        calculator.emplace<PairedRequestCost>();
        calculator.emplace<SameVehicleCost>();
        calculator.emplace<PickupBeforeDropoffCost>();
        calculator.emplace<VehicleCapacityCost>();
        calculator.emplace<MaxEndTimeCost>();
        const Score score = scoreWith(calculator, plan);
        check(score.hard == full.hard && score.medium == 0 && score.soft == 0,
              "the five hard components alone reproduce the hard level");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<UnassignedRequestsCost>();
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 0 && score.medium == full.medium && score.soft == 0,
              "unassignedRequestsCost alone reproduces the medium level");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<TravelTimeCost>();
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 0 && score.medium == 0 && score.soft == full.soft,
              "travelTimeCost alone reproduces the soft level");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides() {
    std::printf("== (d) Weight / level overrides ==\n");
    PdpRoutePlan plan = buildRandomInstance(16, 3, /*seed=*/31);
    std::mt19937_64 rng(7);
    randomizeRoutes(plan, rng, /*poolRate=*/0.05);

    DecomposedCalculator baseline;
    baseline.emplace<VehicleCapacityCost>();
    const Score base = scoreWith(baseline, plan);

    {
        DecomposedCalculator calculator;
        calculator.emplace<VehicleCapacityCost>(10.0);
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 10 * base.hard, "vehicleCapacityCost re-weighted to 10");
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<VehicleCapacityCost>();
        component->level = optimaru::Level::Soft;
        calculator.add(std::move(component));
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 0 && score.medium == 0 && score.soft == base.hard,
              "vehicleCapacityCost demoted to soft");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraint_totals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals() {
    std::printf("== (e) constraintTotals() explains the score ==\n");
    PdpRoutePlan plan = buildRandomInstance(14, 3, /*seed=*/9);
    std::mt19937_64 rng(5);
    randomizeRoutes(plan, rng, /*poolRate=*/0.10);

    const std::size_t componentCount = defaultConstraints().size();
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(plan);
    const optimaru::Score score = calculator.score();

    // A medium total is in medium units (not multiplied by the fold), so it
    // is folded here the way the calculator folds its medium accumulator.
    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal& total : calculator.constraint_totals()) {
        if (total.level == optimaru::Level::Hard)
            hard += total.cost;
        else if (total.level == optimaru::Level::Medium)
            soft += total.cost * static_cast<double>(kMediumUnit);
        else
            soft += total.cost;
    }
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9 &&
                  std::fabs(-soft - score.soft) < 1e-3,
          "per-constraint totals sum to the reported score");
    check(calculator.constraint_totals().size() == componentCount,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) ScoreChecks::Full solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve() {
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    PdpRoutePlan plan = buildRandomInstance(14, 3, /*seed=*/9);
    plan.rebuildUnassignedPool();

    Solver::Config config;
    config.time_limit = std::chrono::seconds(2);
    config.seed = 3;
    config.score_checks = optimaru::ScoreChecks::Full;
    config.ruin_recreate_unimproved_step_trigger = 200;
    config.ruin_recreate_polish_steps = 100;

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    bool corrupted = false;
    try {
        Solver solver(config);
        solver.solve(plan, calculator);
    } catch (const optimaru::ScoreMismatchError& e) {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted) {
        const Score recomputed = ScoreCalculator::score(plan);
        check(plan.score == recomputed, "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) The pair move takes a misplaced bound request straight to its vehicle.
// -------------------------------------------------------------------------
void verifyRepairHonoursRequiredVehicle() {
    std::printf("== (g) Repair and requiredVehicleCost ==\n");
    // A single request, so no stop is routed near it and the pair move falls
    // back on picking a vehicle. With only that move and a handful of steps, a
    // pick among all the vehicles would almost never find the right one.
    const int vehicleCount = 40;
    int repaired = 0;
    const int seeds = 5;
    for (int seed = 0; seed < seeds; ++seed) {
        PdpRoutePlan plan = buildRandomInstance(1, vehicleCount, /*seed=*/21);
        plan.deliveries[0].requiredVehicleIndex = vehicleCount - 1;
        plan.vehicles[0].nodeIndices = {plan.deliveries[0].pickupNodeIndex,
                                        plan.deliveries[0].dropoffNodeIndex};

        Solver::Config config;
        config.step_limit = 8;
        config.time_limit = std::chrono::minutes(1);  // safety net only
        config.seed = static_cast<std::uint64_t>(seed);
        config.pair_assignment_probability = 0.0;
        config.relocate_weight = 0.0;
        config.list_swap_weight = 0.0;
        config.two_opt_weight = 0.0;
        config.or_opt_weight = 0.0;
        config.tail_swap_weight = 0.0;
        config.ruin_recreate = false;

        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        Solver(config).solve(plan, calculator);
        if (plan.vehicles[vehicleCount - 1].nodeIndices.size() == 2) ++repaired;
    }
    check(repaired == seeds, "a misplaced bound request reaches its vehicle");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) The score keeps pinned stops and bound requests where they belong, even
// when something else is already broken.
// -------------------------------------------------------------------------
Solver::Config stepLimited(std::uint64_t seed) {
    Solver::Config config;
    config.step_limit = 20000;
    config.time_limit = std::chrono::minutes(1);  // safety net only
    config.seed = seed;
    return config;
}

void verifyPinnedAndBoundUnderPressure() {
    std::printf("== (h) Pinned stops and bound requests under pressure ==\n");
    int pinnedHeld = 0;
    int stayedOff = 0;
    const int seeds = 5;
    for (int seed = 0; seed < seeds; ++seed) {
        // Both pickups are pinned, the second one days late behind the first.
        // Swapping them would save far more lateness, but the pins must hold.
        PdpRoutePlan late = buildRandomInstance(2, 1, /*seed=*/23);
        const int first = late.deliveries[0].pickupNodeIndex;
        const int second = late.deliveries[1].pickupNodeIndex;
        for (int nodeIndex : {first, late.deliveries[0].dropoffNodeIndex}) {
            late.nodes[nodeIndex].minStartTime = 500000;
            late.nodes[nodeIndex].maxEndTime = 600000;
        }
        for (int nodeIndex : {second, late.deliveries[1].dropoffNodeIndex}) {
            late.nodes[nodeIndex].minStartTime = 0;
            late.nodes[nodeIndex].maxEndTime = 40000;
        }
        late.vehicles[0].nodeIndices = {first, second, late.deliveries[0].dropoffNodeIndex,
                                        late.deliveries[1].dropoffNodeIndex};
        late.vehicles[0].pinnedNodeIndices = {first, second};
        DecomposedCalculator lateCalculator;
        addDefaultConstraints(lateCalculator);
        Solver(stepLimited(seed)).solve(late, lateCalculator);
        const std::vector<int>& route = late.vehicles[0].nodeIndices;
        if (route.size() >= 2 && route[0] == first && route[1] == second) ++pinnedHeld;

        // Vehicle 1 cannot reach the request in time and vehicle 0 is not
        // allowed to take it.
        PdpRoutePlan bound = buildRandomInstance(1, 2, /*seed=*/23);
        bound.deliveries[0].requiredVehicleIndex = 1;
        for (Seconds& seconds : bound.drivingSeconds[1]) seconds = 10000000;
        DecomposedCalculator boundCalculator;
        addDefaultConstraints(boundCalculator);
        Solver(stepLimited(seed)).solve(bound, boundCalculator);
        if (bound.vehicles[0].nodeIndices.empty()) ++stayedOff;
    }
    check(pinnedHeld == seeds, "lateness never pays for moving a pinned stop");
    check(stayedOff == seeds, "a bound request does not ride another vehicle");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) Raw costs by hand: one positive and one zero case per component.
// -------------------------------------------------------------------------
// Two requests, two vehicles, an explicit driving matrix of 100 s per index
// step (location k at longitude 0.001 * k on the equator, home = location 0):
//   r1 = p1 (loc 1) -> d1 (loc 2), r2 = p2 (loc 3) -> d2 (loc 4), demand 4
//   each, 60 s of service at every stop, windows [0, 24 h], vehicles of
//   capacity 10 leaving at 0. With vehicle 0 on p1 d1 p2 d2 the arrivals are
//   100, 260, 420, 580, the route ends at 640 and drives 400 s (open route).
PdpRoutePlan buildHandInstance() {
    PdpRoutePlan plan;
    plan.name = "hand";
    for (int k = 0; k < 5; ++k) plan.locations.push_back(Location{0.0, 0.001 * k, k});
    for (int v = 0; v < 2; ++v) {
        Vehicle vehicle;
        vehicle.id = std::to_string(v);
        vehicle.capacity = 10;
        vehicle.homeLocationIndex = 0;
        plan.vehicles.push_back(vehicle);
    }
    const char* ids[] = {"p1", "d1", "p2", "d2"};
    for (int n = 0; n < 4; ++n)
        plan.nodes.push_back(
                Node{ids[n], n + 1, 0, 24 * 3600, 60, n / 2, n % 2 == 0, -1, NULL_TIME});
    plan.deliveries = {Delivery{"r1", 0, 1, 4, 0, -1}, Delivery{"r2", 2, 3, 4, 0, -1}};
    plan.drivingSeconds.assign(5, std::vector<Seconds>(5, 0));
    for (int a = 0; a < 5; ++a)
        for (int b = 0; b < 5; ++b) plan.drivingSeconds[a][b] = 100 * std::abs(a - b);
    return plan;
}

// Vehicle 0 serves p1 d1 p2 d2; vehicle 1 and the pool are empty.
PdpRoutePlan handRouted() {
    PdpRoutePlan plan = buildHandInstance();
    plan.vehicles[0].nodeIndices = {0, 1, 2, 3};
    return plan;
}

// The raw per-unit costs (vehicle 0, vehicle 1, pool; one entry for a global)
// of one registry name, read off the default assembly.
std::vector<double> rawCosts(PdpRoutePlan plan, const std::string& name) {
    plan.rebuildUnassignedPool();
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(plan);
    calculator.score();
    for (const auto& entry : calculator.constraint_costs())
        if (entry.name == name) return entry.unit_costs;
    throw std::logic_error("no constraint " + name);
}

bool near(const std::vector<double>& got, const std::vector<double>& want) {
    if (got.size() != want.size()) return false;
    for (std::size_t i = 0; i < got.size(); ++i)
        if (std::fabs(got[i] - want[i]) > 1e-6 * std::max(1.0, std::fabs(want[i]))) return false;
    return true;
}

void byHand(const std::string& name, const std::function<void(PdpRoutePlan&)>& positive,
            const std::vector<double>& positiveCosts,
            const std::function<void(PdpRoutePlan&)>& zero, const std::vector<double>& zeroCosts) {
    PdpRoutePlan up = handRouted();
    positive(up);
    check(near(rawCosts(up, name), positiveCosts), name + " positive");
    PdpRoutePlan down = handRouted();
    zero(down);
    check(near(rawCosts(down, name), zeroCosts), name + " zero");
}

void verifyRawCostsByHand() {
    std::printf("== (i) Raw costs by hand ==\n");
    constexpr double pi = 3.14159265358979323846;
    // On the equator the haversine distance is R * dLongitude (radians), and
    // the equirectangular projection keeps the stops on one line.
    const double meterPerStep = 6371000.0 * 0.001 * pi / 180.0;
    const std::vector<double> none{0.0, 0.0, 0.0};
    const std::vector<double> noGlobal{0.0};
    const auto keep = [](PdpRoutePlan&) {};

    // Capacity 3: the load reaches 4 after p1 and after p2 -> 1 + 1.
    byHand(
            "vehicleCapacityCost", [](PdpRoutePlan& p) { p.vehicles[0].capacity = 3; },
            {2.0, 0.0, 0.0}, keep, none);
    // p2 finishes at 480 against 400 (80 s -> 2 min), d2 at 640 against 580
    // (60 s -> 1 min).
    byHand(
            "maxEndTimeCost",
            [](PdpRoutePlan& p) {
                p.nodes[2].maxEndTime = 400;
                p.nodes[3].maxEndTime = 580;
            },
            {3.0, 0.0, 0.0}, [](PdpRoutePlan& p) { p.nodes[3].maxEndTime = 640; }, none);
    // 4 legs of 100 s, open; returnToDepot adds d2 -> home = 400 s.
    byHand(
            "travelTimeCost", keep, {400.0, 0.0, 0.0},
            [](PdpRoutePlan& p) { p.vehicles[0].nodeIndices.clear(); }, none);
    byHand(
            "travelTimeCost", [](PdpRoutePlan& p) { p.vehicles[0].returnToDepot = true; },
            {800.0, 0.0, 0.0},
            [](PdpRoutePlan& p) {
                p.vehicles[0].returnToDepot = true;
                p.vehicles[0].nodeIndices.clear();  // a parked vehicle drives 0
            },
            none);
    byHand(
            "maxTravelTimeCost", [](PdpRoutePlan& p) { p.vehicles[0].maxTravelTime = 300; },
            {100.0, 0.0, 0.0}, [](PdpRoutePlan& p) { p.vehicles[0].maxTravelTime = 400; }, none);
    byHand(
            "maxRouteDurationCost", [](PdpRoutePlan& p) { p.vehicles[0].maxRouteDuration = 600; },
            {40.0, 0.0, 0.0}, [](PdpRoutePlan& p) { p.vehicles[0].maxRouteDuration = 640; }, none);
    byHand(
            "vehicleMaxEndTimeCost", [](PdpRoutePlan& p) { p.vehicles[0].maxEndTime = 600; },
            {40.0, 0.0, 0.0}, [](PdpRoutePlan& p) { p.vehicles[0].maxEndTime = 640; }, none);
    byHand(
            "maxStopsCost", [](PdpRoutePlan& p) { p.vehicles[0].maxStops = 3; }, {1.0, 0.0, 0.0},
            [](PdpRoutePlan& p) { p.vehicles[0].maxStops = 4; }, none);
    // The farthest stop (d2) is 4 steps from home.
    byHand(
            "maxRouteRadiusCost",
            [&](PdpRoutePlan& p) { p.vehicles[0].maxRouteRadiusMeters = 4 * meterPerStep - 25.0; },
            {25.0, 0.0, 0.0},
            [&](PdpRoutePlan& p) { p.vehicles[0].maxRouteRadiusMeters = 4 * meterPerStep + 1.0; },
            none);
    // Four stops on one line, 3 steps from the first to the last: radius 1.5.
    byHand(
            "enclosingCircleRadiusCost", keep, {1.5 * meterPerStep, 0.0, 0.0},
            [](PdpRoutePlan& p) { p.vehicles[0].nodeIndices = {0}; }, none);
    // Target 10 * 1.0 against 4 + 4 delivered.
    byHand(
            "minCapacityUtilizationCost", [](PdpRoutePlan& p) { p.vehicles[0].minLoadRatio = 1.0; },
            {2.0, 0.0, 0.0}, [](PdpRoutePlan& p) { p.vehicles[0].minLoadRatio = 0.5; }, none);
    byHand(
            "routeDurationCost", keep, {640.0, 0.0, 0.0},
            [](PdpRoutePlan& p) { p.vehicles[0].nodeIndices.clear(); }, none);
    // 100 + 260 + 420 + 580.
    byHand(
            "latencyCost", keep, {1360.0, 0.0, 0.0},
            [](PdpRoutePlan& p) { p.vehicles[0].nodeIndices.clear(); }, none);
    // d1 finishes at 320, d2 at 640, both ordered at 0: 320^2 + 640^2. Zero:
    // each request ordered exactly when its dropoff finishes.
    byHand(
            "squaredOrderAgeCost", keep, {512000.0, 0.0, 0.0},
            [](PdpRoutePlan& p) {
                p.deliveries[0].orderTime = 320;
                p.deliveries[1].orderTime = 640;
            },
            none);
    // In the pool every dropoff is charged as finishing at 3600: 2 * 3600^2.
    byHand(
            "squaredOrderAgeCost", [](PdpRoutePlan& p) { p.vehicles[0].nodeIndices.clear(); },
            {0.0, 0.0, 25920000.0},
            [](PdpRoutePlan& p) {
                p.vehicles[0].nodeIndices.clear();
                p.deliveries[0].orderTime = 3600;
                p.deliveries[1].orderTime = 3600;
            },
            none);
    byHand(
            "vehicleFixedCost", [](PdpRoutePlan& p) { p.vehicles[0].fixedCost = 50.0; },
            {50.0, 0.0, 0.0},
            [](PdpRoutePlan& p) {
                p.vehicles[1].fixedCost = 50.0;  // vehicle 1 stays unused
            },
            none);
    // Pinned d1, p1 against a route starting p1, d1: both positions wrong.
    byHand(
            "pinnedStopsCost", [](PdpRoutePlan& p) { p.vehicles[0].pinnedNodeIndices = {1, 0}; },
            {2.0, 0.0, 0.0}, [](PdpRoutePlan& p) { p.vehicles[0].pinnedNodeIndices = {0, 1}; },
            none);
    // r1 bound to vehicle 1 rides vehicle 0 (2); r2 bound to vehicle 1 waits
    // in the pool (1).
    byHand(
            "requiredVehicleCost",
            [](PdpRoutePlan& p) {
                p.deliveries[0].requiredVehicleIndex = 1;
                p.deliveries[1].requiredVehicleIndex = 1;
                p.vehicles[0].nodeIndices = {0, 1};
            },
            {2.0, 0.0, 1.0}, [](PdpRoutePlan& p) { p.deliveries[0].requiredVehicleIndex = 0; },
            none);
    // d2 left out of the route: r2 is half-routed.
    byHand(
            "pairedRequestCost", [](PdpRoutePlan& p) { p.vehicles[0].nodeIndices = {0, 1, 2}; },
            {1.0}, keep, noGlobal);
    // d2 rides vehicle 1.
    byHand(
            "sameVehicleCost",
            [](PdpRoutePlan& p) {
                p.vehicles[0].nodeIndices = {0, 1, 2};
                p.vehicles[1].nodeIndices = {3};
            },
            {1.0}, keep, noGlobal);
    byHand(
            "pickupBeforeDropoffCost",
            [](PdpRoutePlan& p) { p.vehicles[0].nodeIndices = {1, 0, 2, 3}; }, {1.0}, keep,
            noGlobal);
    // Both requests unrouted: (60 + 60) / 60 = 2 minutes each.
    byHand(
            "unassignedRequestsCost", [](PdpRoutePlan& p) { p.vehicles[0].nodeIndices.clear(); },
            {4.0}, keep, noGlobal);
    // r1 and r2 are one group: r2 rides vehicle 1 while r1 rides vehicle 0, so
    // one of the two is apart. Together on vehicle 0 they are whole.
    const auto grouped = [](PdpRoutePlan& p) {
        p.deliveries[0].groupIndex = 5;
        p.deliveries[1].groupIndex = 5;
    };
    byHand(
            "requestGroupCost",
            [&grouped](PdpRoutePlan& p) {
                grouped(p);
                p.vehicles[0].nodeIndices = {0, 1};
                p.vehicles[1].nodeIndices = {2, 3};
            },
            {1.0}, grouped, noGlobal);
    // One of the group waits while the other rides: apart as well.
    byHand(
            "requestGroupCost",
            [&grouped](PdpRoutePlan& p) {
                grouped(p);
                p.vehicles[0].nodeIndices = {0, 1};
            },
            {1.0},
            [&grouped](PdpRoutePlan& p) {
                grouped(p);
                p.vehicles[0].nodeIndices.clear();  // both wait: whole
            },
            noGlobal);
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) The catalog read off the default assembly == the components' constants.
// -------------------------------------------------------------------------
template <typename Component>
bool rowIs(const optimaru::ConstraintInfo& row, const char* name, optimaru::Level level,
           double weight, bool is_global) {
    return row.name == name && row.default_level == Component::kDefaultLevel &&
           row.default_weight == Component::kDefaultWeight && row.default_level == level &&
           row.default_weight == weight && row.level_flippable && !row.reward &&
           row.is_global == is_global && row.default_enabled == (weight > 0.0);
}

void verifyCatalog() {
    std::printf("== (j) Catalog == component constants ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 21, "21 catalog rows");
    if (rows.size() != 21) return;
    // Unit constraints first, then the globals, in emplace order: the
    // list_constraints() order.
    check(rowIs<VehicleCapacityCost>(rows[0], "vehicleCapacityCost", Level::Hard, 1.0, false) &&
                  rowIs<MaxEndTimeCost>(rows[1], "maxEndTimeCost", Level::Hard, 1.0, false) &&
                  rowIs<TravelTimeCost>(rows[2], "travelTimeCost", Level::Soft, 1.0, false) &&
                  rowIs<MaxTravelTimeCost>(rows[3], "maxTravelTimeCost", Level::Hard, 1.0, false) &&
                  rowIs<MaxRouteDurationCost>(rows[4], "maxRouteDurationCost", Level::Hard, 1.0,
                                              false) &&
                  rowIs<VehicleMaxEndTimeCost>(rows[5], "vehicleMaxEndTimeCost", Level::Hard, 1.0,
                                               false) &&
                  rowIs<MaxStopsCost>(rows[6], "maxStopsCost", Level::Hard, 1.0, false) &&
                  rowIs<MaxRouteRadiusCost>(rows[7], "maxRouteRadiusCost", Level::Hard, 1.0,
                                            false) &&
                  rowIs<EnclosingCircleRadiusCost>(rows[8], "enclosingCircleRadiusCost",
                                                   Level::Soft, 0.0, false) &&
                  rowIs<MinCapacityUtilizationCost>(rows[9], "minCapacityUtilizationCost",
                                                    Level::Soft, 1.0, false) &&
                  rowIs<RouteDurationCost>(rows[10], "routeDurationCost", Level::Soft, 0.0,
                                           false) &&
                  rowIs<LatencyCost>(rows[11], "latencyCost", Level::Soft, 0.0, false) &&
                  rowIs<SquaredOrderAgeCost>(rows[12], "squaredOrderAgeCost", Level::Soft, 0.0,
                                             false) &&
                  rowIs<VehicleFixedCost>(rows[13], "vehicleFixedCost", Level::Soft, 1.0, false) &&
                  rowIs<PinnedStopsCost>(rows[14], "pinnedStopsCost", Level::Hard, 1e12, false) &&
                  rowIs<RequiredVehicleCost>(rows[15], "requiredVehicleCost", Level::Hard, 1000.0,
                                             false) &&
                  rowIs<PairedRequestCost>(rows[16], "pairedRequestCost", Level::Hard, 1000.0,
                                           true) &&
                  rowIs<SameVehicleCost>(rows[17], "sameVehicleCost", Level::Hard, 1000.0, true) &&
                  rowIs<PickupBeforeDropoffCost>(rows[18], "pickupBeforeDropoffCost", Level::Hard,
                                                 1000.0, true) &&
                  rowIs<RequestGroupCost>(rows[19], "requestGroupCost", Level::Hard, 1000.0,
                                          true) &&
                  rowIs<UnassignedRequestsCost>(rows[20], "unassignedRequestsCost", Level::Medium,
                                                1.0, true),
          "info() == the components' kDefaultWeight / kDefaultLevel");
    check(PairedRequestCost::kDefaultWeight == ScoreCalculator::kCouplingPenalty &&
                  RequiredVehicleCost::kDefaultWeight == ScoreCalculator::kCouplingPenalty &&
                  RequestGroupCost::kDefaultWeight == ScoreCalculator::kCouplingPenalty &&
                  PinnedStopsCost::kDefaultWeight == ScoreCalculator::kPinnedPenalty,
          "the penalty defaults are ScoreCalculator's constants");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (k) The medium level: unassignedRequestsCost.
// -------------------------------------------------------------------------
optimaru::DecomposedObjective<PdpRoutePlan>::LevelScores levelsWith(ConstraintAssembly assembly,
                                                                    PdpRoutePlan& plan) {
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    calculator.rebuild(plan);
    calculator.score();
    return calculator.level_scores();
}

void verifyMediumLevel() {
    std::printf("== (k) Medium level ==\n");
    // Vehicle 0 serves r1; r2 is dropped: 2 medium minutes, 200 s of driving.
    PdpRoutePlan plan = buildHandInstance();
    plan.vehicles[0].nodeIndices = {0, 1};
    plan.rebuildUnassignedPool();
    const Score monolithic = ScoreCalculator::score(plan);
    check(monolithic.hard == 0 && monolithic.medium == -2 && monolithic.soft == -200,
          "the hand plan drops 2 medium minutes");

    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        calculator.rebuild(plan);
        const optimaru::Score engine = calculator.score();
        check(engine.hard == 0 && engine.soft == -200.0 - 2.0 * static_cast<double>(kMediumUnit),
              "default: medium folded into soft x 10^9");
        check(engine.hard == toEngineScore(monolithic).hard &&
                      engine.soft == toEngineScore(monolithic).soft,
              "default: the fold equals the monolithic Score adapter");
        const auto levels = calculator.level_scores();
        check(levels.hard == 0 && levels.medium == -2.0 && levels.soft == -200.0,
              "levelScores() reports the unfolded levels");
    }
    {
        const auto levels = levelsWith(defaultConstraints().hard("unassignedRequestsCost"), plan);
        check(levels.hard == -2 && levels.medium == 0.0 && levels.soft == -200.0,
              "hard(): one hard unit per raw minute");
    }
    {
        const auto levels = levelsWith(defaultConstraints().soft("unassignedRequestsCost"), plan);
        check(levels.hard == 0 && levels.medium == 0.0 && levels.soft == -202.0,
              "soft(): one soft unit per raw minute, outside the fold");
    }
    {
        const auto levels =
                levelsWith(defaultConstraints().weight("unassignedRequestsCost", 3), plan);
        check(levels.hard == 0 && levels.medium == -6.0 && levels.soft == -200.0,
              "weight 3 stays in medium units");
    }
    {
        const auto levels = levelsWith(
                defaultConstraints().level("travelTimeCost", optimaru::Level::Medium), plan);
        check(levels.hard == 0 && levels.medium == -202.0 && levels.soft == 0.0,
              "level(Medium): travelTimeCost moves onto medium, one medium unit per second");
    }
    {
        const auto levels = levelsWith(defaultConstraints()
                                               .level("travelTimeCost", optimaru::Level::Medium)
                                               .weight("travelTimeCost", 0.5),
                                       plan);
        check(levels.hard == 0 && levels.medium == -102.0 && levels.soft == 0.0,
              "a weight on a moved constraint is in medium units");
    }
    std::printf("\n");
}

// Incremental == full recompute with the medium term overridden, through the
// Editor protocol (the coupling globals have no preview contract, so
// the engine applies and scores every move; this walk is that path).
void verifyMediumIncremental() {
    std::printf("== (k) Medium overrides: incremental == full ==\n");
    const std::vector<std::pair<std::string, std::function<ConstraintAssembly()>>> variants{
            {"default", [] { return defaultConstraints(); }},
            {"hard", [] { return defaultConstraints().hard("unassignedRequestsCost"); }},
            {"soft", [] { return defaultConstraints().soft("unassignedRequestsCost"); }},
            {"weight 3", [] { return defaultConstraints().weight("unassignedRequestsCost", 3); }},
            {"with travelTimeCost on medium",
             [] { return defaultConstraints().level("travelTimeCost", optimaru::Level::Medium); }},
    };
    using Descriptor = optimaru::SolutionTraits<PdpRoutePlan>;
    for (const auto& variant : variants) {
        PdpRoutePlan plan = buildRandomInstance(12, 3, /*seed=*/17);
        plan.rebuildUnassignedPool();
        DecomposedCalculator calculator;
        variant.second().into(calculator);
        optimaru::Editor<PdpRoutePlan> editor(plan, calculator);
        std::mt19937_64 rng(41);
        int mismatches = 0;
        int mediumSteps = 0;
        for (int step = 0; step < 600; ++step) {
            const int list_count = Descriptor::list_count(plan);
            const int from = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
            if (Descriptor::list_size(plan, from) == 0) continue;
            const int position = static_cast<int>(
                    rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, from)));
            const int element = Descriptor::element_at(plan, from, position);
            editor.remove(from, position);
            const int to = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
            const int insert_at = static_cast<int>(
                    rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, to) + 1));
            editor.insert(to, insert_at, element);

            const optimaru::Score incremental = editor.score();
            DecomposedCalculator fresh;
            variant.second().into(fresh);
            fresh.rebuild(plan);
            const optimaru::Score full = fresh.score();
            if (fresh.level_scores().medium != 0.0 || calculator.level_scores().medium != 0.0)
                ++mediumSteps;
            if (incremental.hard != full.hard || std::fabs(incremental.soft - full.soft) > 1e-6)
                ++mismatches;
        }
        check(mismatches == 0, "unassignedRequestsCost " + variant.first +
                                       ": 600 random list moves, incremental == full recompute");
        if (variant.first != "hard" && variant.first != "soft")
            check(mediumSteps > 0, variant.first + ": the walk visits medium states");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (l) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with add_instances(), as the Python bindings
// build it from a pick list.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string& name) {
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// The hand instance with a third vehicle and a third request r3 (p3 at
// location 3, d3 at location 4, 120 s of service each: 4 penalty minutes):
// vehicle 0 (capacity 2) on p1 d1 drives 200 s and carries 4 (2 over after
// p1), vehicle 1 on p2 d2 drives 400 s, vehicle 2 stays idle, r3 waits in
// the pool. Raw per unit (3 vehicles, pool): travel [200, 400, 0, 0],
// capacity [2, 0, 0, 0]; unassigned total 4.
PdpRoutePlan repeatInstance() {
    PdpRoutePlan plan = buildHandInstance();
    Vehicle idle = plan.vehicles[1];
    idle.id = "2";
    plan.vehicles.push_back(idle);
    plan.nodes.push_back(Node{"p3", 3, 0, 24 * 3600, 120, 2, true, -1, NULL_TIME});
    plan.nodes.push_back(Node{"d3", 4, 0, 24 * 3600, 120, 2, false, -1, NULL_TIME});
    plan.deliveries.push_back(Delivery{"r3", 4, 5, 4, 0, -1});
    plan.vehicles[0].capacity = 2;
    plan.vehicles[0].nodeIndices = {0, 1};
    plan.vehicles[1].nodeIndices = {2, 3};
    plan.rebuildUnassignedPool();
    return plan;
}

DecomposedCalculator::LevelScores levelsOf(ConstraintAssembly assembly, PdpRoutePlan plan) {
    return levelsWith(std::move(assembly), plan);
}

// Every shaped flavour at once: unit constraints evaluating the vehicles
// (travel, capacity, lateness, latency), one evaluating the pool too
// (squaredOrderAgeCost), the medium global and a coupling global.
ConstraintAssembly repeatedAssembly() {
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("travelTimeCost", 3000.0).clip("travelTimeCost", false);
    assembly.offset("unassignedRequestsCost", 1.0);
    assembly.add_instances(std::move(copyOf("travelTimeCost")
                                             .weight("travelTimeCost", 0.001)
                                             .exponent("travelTimeCost", 2.0)
                                             .offset("travelTimeCost", 2000.0)));
    // The literal copies of hard components sit on soft: hard refuses the
    // literal formula with an offset (hard 0 must mean no violation).
    assembly.add_instances(std::move(copyOf("vehicleCapacityCost")
                                             .soft("vehicleCapacityCost")
                                             .weight("vehicleCapacityCost", 2.0)
                                             .offset("vehicleCapacityCost", 1.0)
                                             .clip("vehicleCapacityCost", false)));
    assembly.add_instances(std::move(copyOf("vehicleCapacityCost")
                                             .weight("vehicleCapacityCost", 2.0)
                                             .offset("vehicleCapacityCost", 1.0)));
    assembly.add_instances(std::move(copyOf("unassignedRequestsCost")
                                             .weight("unassignedRequestsCost", 2.0)
                                             .offset("unassignedRequestsCost", 3.0)
                                             .clip("unassignedRequestsCost", false)));
    assembly.add_instances(std::move(copyOf("maxEndTimeCost").offset("maxEndTimeCost", -2.0)));
    assembly.add_instances(std::move(copyOf("latencyCost")
                                             .weight("latencyCost", 0.01)
                                             .offset("latencyCost", 5000.0)
                                             .clip("latencyCost", false)));
    assembly.add_instances(std::move(copyOf("squaredOrderAgeCost")
                                             .weight("squaredOrderAgeCost", 1e-6)
                                             .offset("squaredOrderAgeCost", 1e6)
                                             .clip("squaredOrderAgeCost", false)));
    assembly.add_instances(std::move(copyOf("pairedRequestCost")
                                             .soft("pairedRequestCost")
                                             .offset("pairedRequestCost", 1.0)
                                             .clip("pairedRequestCost", false)));
    assembly.add_instances(std::move(copyOf("pairedRequestCost").offset("pairedRequestCost", 1.0)));
    return assembly;
}

void verifyRepeatsAndOffsets() {
    std::printf("== (l) Repeated components, offset / clip ==\n");
    {
        PdpRoutePlan plan = repeatInstance();
        ConstraintAssembly assembly = copyOf("travelTimeCost");
        assembly.add_instances(std::move(copyOf("travelTimeCost")
                                                 .weight("travelTimeCost", 0.5)
                                                 .exponent("travelTimeCost", 2.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        calculator.rebuild(plan);
        const optimaru::Score score = calculator.score();
        check(score.hard == 0 && score.soft == -(600.0 + 0.5 * (200.0 * 200.0 + 400.0 * 400.0)),
              "travel + 0.5 x travel^2 per vehicle: -(600 + 100000) soft");
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].unit_costs == entries[1].unit_costs &&
                      entries[0].unit_costs == std::vector<double>{200.0, 400.0, 0.0, 0.0} &&
                      entries[1].weight == 0.5 && entries[1].exponent == 2.0 &&
                      entries[1].default_weight == 1.0,
              "two instances, the same raw costs, each with its own knobs");
        check(entries.size() == 2 && entries[0].scoped && entries[1].scoped &&
                      entries[0].units == std::vector<int>{0, 1, 2} &&
                      entries[1].units == std::vector<int>{0, 1, 2},
              "a per-vehicle component evaluates the three vehicles, not the pool");
    }
    {
        check(levelsOf(copyOf("travelTimeCost").offset("travelTimeCost", 300.0), repeatInstance())
                              .soft == -100.0,
              "threshold offset 300: 100");
        // Literal: (200 - 300) + (400 - 300) + (0 - 300) = -300, the idle
        // vehicle included and the pool not (it would make -600).
        check(levelsOf(copyOf("travelTimeCost")
                               .offset("travelTimeCost", 300.0)
                               .clip("travelTimeCost", false),
                       repeatInstance())
                              .soft == 300.0,
              "literal offset 300: -100 + 100 - 300 -> +300 soft, the pool not charged");
        check(levelsOf(copyOf("travelTimeCost").offset("travelTimeCost", -50.0), repeatInstance())
                              .soft == -700.0,
              "threshold offset -50: 250 + 450, the idle vehicle stays 0");
        check(levelsOf(copyOf("travelTimeCost")
                               .offset("travelTimeCost", -50.0)
                               .clip("travelTimeCost", false),
                       repeatInstance())
                              .soft == -750.0,
              "literal offset -50: 250 + 450 + 50");
        // The literal capacity pick on soft: (2 - 2.5) + (0 - 2.5) x 2 =
        // -5.5, the idle vehicle included: +5.5 soft (not rounded).
        const auto literal = levelsOf(copyOf("vehicleCapacityCost")
                                              .soft("vehicleCapacityCost")
                                              .offset("vehicleCapacityCost", 2.5)
                                              .clip("vehicleCapacityCost", false),
                                      repeatInstance());
        check(literal.hard == 0 && literal.soft == 5.5,
              "capacity literal offset 2.5 on soft: -0.5 - 2.5 - 2.5 -> +5.5 soft");
        // A hard threshold pick rounds per vehicle: travel moved to hard at
        // weight 0.005 beyond 100 s: llround(0.5) + llround(1.5) = 3 (a
        // rounded total would give 2).
        check(levelsOf(copyOf("travelTimeCost")
                               .hard("travelTimeCost")
                               .weight("travelTimeCost", 0.005)
                               .offset("travelTimeCost", 100.0),
                       repeatInstance())
                              .hard == -3,
              "travel on hard, threshold offset 100 at weight 0.005: llround per vehicle, -3 hard");
        // requiredVehicleCost prices the pool: r3 bound to vehicle 0 and
        // waiting there costs 1 on the pool's unit, and a literal offset 2
        // (on soft: hard refuses it) charges all four units: 3 x (0 - 2) +
        // (1 - 2) = -7.
        PdpRoutePlan bound = repeatInstance();
        bound.deliveries[2].requiredVehicleIndex = 0;
        DecomposedCalculator required;
        copyOf("requiredVehicleCost")
                .soft("requiredVehicleCost")
                .weight("requiredVehicleCost", 1.0)
                .offset("requiredVehicleCost", 2.0)
                .clip("requiredVehicleCost", false)
                .into(required);
        required.rebuild(bound);
        const optimaru::Score requiredScore = required.score();
        check(requiredScore.hard == 0 && requiredScore.soft == 7.0 &&
                      !required.constraint_costs()[0].scoped &&
                      required.constraint_costs()[0].unit_costs ==
                              std::vector<double>{0.0, 0.0, 0.0, 1.0},
              "requiredVehicleCost evaluates the pool too: literal offset 2 on soft -> +7 soft");
        // The global's total is shaped once: (4 - 2)^2 = 4 medium minutes.
        const auto global = levelsOf(copyOf("unassignedRequestsCost")
                                             .offset("unassignedRequestsCost", 2.0)
                                             .exponent("unassignedRequestsCost", 2.0),
                                     repeatInstance());
        check(global.medium == -4.0 && global.hard == 0 && global.soft == 0.0,
              "unassigned threshold offset 2, exponent 2: -(4 - 2)^2 medium");
        const auto reward = levelsOf(copyOf("unassignedRequestsCost")
                                             .offset("unassignedRequestsCost", 6.0)
                                             .clip("unassignedRequestsCost", false),
                                     repeatInstance());
        check(reward.medium == 2.0, "unassigned literal offset 6: 4 - 6 -> +2 medium");
        const auto twice = levelsOf(
                std::move(copyOf("unassignedRequestsCost")
                                  .add_instances(std::move(
                                          copyOf("unassignedRequestsCost")
                                                  .weight("unassignedRequestsCost", 2.0)))),
                repeatInstance());
        check(twice.medium == -12.0, "a repeated medium pick: -4 - 8 medium");
    }
    {
        // On a level that counts toward the hard score the literal formula
        // with an offset is refused at reset, whichever sign and setter
        // order: +2.5 would reward the vehicles below it (+7 hard, so one
        // vehicle's spare capacity would cancel another's overload), -2.5
        // would charge three vehicles that hold no violation. Medium folds
        // into soft here, so it keeps the literal formula.
        auto resetRefusal = [](ConstraintAssembly assembly) {
            PdpRoutePlan plan = repeatInstance();
            try {
                levelsWith(std::move(assembly), plan);
            } catch (const std::logic_error& error) {
                return std::string(error.what());
            }
            return std::string();
        };
        auto refusal = [](const std::string& name) {
            return "Constraint '" + name +
                   "' cannot use clip=false with a non-zero offset on a level that counts toward "
                   "the hard score (hard 0 must mean no violation)";
        };
        check(resetRefusal(copyOf("vehicleCapacityCost")
                                   .offset("vehicleCapacityCost", 2.5)
                                   .clip("vehicleCapacityCost", false)) ==
                              refusal("vehicleCapacityCost") &&
                      resetRefusal(copyOf("vehicleCapacityCost")
                                           .clip("vehicleCapacityCost", false)
                                           .offset("vehicleCapacityCost", -2.5)) ==
                              refusal("vehicleCapacityCost"),
              "capacity literal offset +-2.5 on hard (its default): refused at reset");
        check(resetRefusal(copyOf("pairedRequestCost")
                                   .offset("pairedRequestCost", 1.0)
                                   .clip("pairedRequestCost", false)) ==
                      refusal("pairedRequestCost"),
              "a coupling global with the literal offset on hard (its default): refused at reset");
        check(resetRefusal(copyOf("travelTimeCost")
                                   .offset("travelTimeCost", 300.0)
                                   .clip("travelTimeCost", false)
                                   .hard("travelTimeCost")) == refusal("travelTimeCost"),
              "travel literal offset moved to hard afterwards: refused at reset");
        ConstraintAssembly repeated = copyOf("vehicleCapacityCost");
        repeated.add_instances(std::move(copyOf("vehicleCapacityCost")
                                                 .offset("vehicleCapacityCost", 1.0)
                                                 .clip("vehicleCapacityCost", false)));
        check(resetRefusal(std::move(repeated)) == refusal("vehicleCapacityCost"),
              "a repeated capacity instance with the literal offset on hard: refused at reset");
        check(resetRefusal(copyOf("vehicleCapacityCost").offset("vehicleCapacityCost", 2.5))
                              .empty() &&
                      resetRefusal(copyOf("vehicleCapacityCost").clip("vehicleCapacityCost", false))
                              .empty() &&
                      resetRefusal(copyOf("unassignedRequestsCost")
                                           .offset("unassignedRequestsCost", 6.0)
                                           .clip("unassignedRequestsCost", false))
                              .empty(),
              "allowed: a threshold offset or clip=false with offset 0 on hard, the literal "
              "offset on the into-soft medium");
    }
    using Descriptor = optimaru::SolutionTraits<PdpRoutePlan>;
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        PdpRoutePlan plan = buildRandomInstance(12, 3, /*seed=*/17);
        std::mt19937_64 layout(29);
        randomizeRoutes(plan, layout, /*poolRate=*/0.15);
        plan.rebuildUnassignedPool();
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::Editor<PdpRoutePlan> editor(plan, calculator);
        std::mt19937_64 rng(43);
        int mismatches = 0;
        double drift = 0.0;
        const int steps = 1500;
        for (int step = 0; step < steps; ++step) {
            const int list_count = Descriptor::list_count(plan);
            const int from = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
            if (Descriptor::list_size(plan, from) == 0) continue;
            const int position = static_cast<int>(
                    rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, from)));
            const int element = Descriptor::element_at(plan, from, position);
            editor.remove(from, position);
            const int to = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
            const int insert_at = static_cast<int>(
                    rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, to) + 1));
            editor.insert(to, insert_at, element);

            const optimaru::Score incremental = editor.score();
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            fresh.rebuild(plan);
            const optimaru::Score full = fresh.score();
            drift = std::max(drift, std::fabs(full.soft - incremental.soft));
            if (!optimaru::scores_agree(incremental, full)) ++mismatches;
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0,
              std::to_string(steps) +
                      " random list moves with repeated, shifted components: incremental == fresh");
    }
    {
        // The coupling globals have no preview contract (the engine applies
        // every move instead), so the previews are checked on the unit
        // components alone, every instance of each kept.
        PdpRoutePlan plan = buildRandomInstance(12, 3, /*seed=*/19);
        std::mt19937_64 layout(31);
        randomizeRoutes(plan, layout, /*poolRate=*/0.10);
        plan.rebuildUnassignedPool();
        DecomposedCalculator calculator;
        repeatedAssembly()
                .only({"vehicleCapacityCost", "maxEndTimeCost", "travelTimeCost", "latencyCost",
                       "squaredOrderAgeCost", "vehicleFixedCost"})
                .into(calculator);
        optimaru::Editor<PdpRoutePlan> editor(plan, calculator, optimaru::ScoreChecks::Full);
        editor.score();
        optimaru::testing::PreviewParity parity;
        bool corrupted = false;
        try {
            parity = optimaru::testing::run_preview_parity(editor, calculator, /*seed=*/13,
                                                           /*trials=*/3000);
        } catch (const optimaru::ScoreMismatchError& error) {
            corrupted = true;
            std::printf("    %s\n", error.what());
        }
        std::printf("    %d moves checked, %d previewed, %d declined\n", parity.checked,
                    parity.previewed, parity.declined);
        check(!corrupted && parity.all_equal() && parity.declined == 0 && parity.previewed > 1000,
              "repeated, shifted unit components: every preview served and equal to the "
              "applied score (FULL_ASSERT)");
    }
    for (const bool grouped : {true, false}) {
        // requestGroupCost answers previews (the other globals of the default
        // set do not): with groups from the candidate members, without any
        // through its piecewise answer of 0. Next to three unit components.
        PdpRoutePlan plan = buildRandomInstance(12, 3, /*seed=*/19);
        if (grouped) {
            for (std::size_t d = 0; d < plan.deliveries.size(); ++d)
                plan.deliveries[d].groupIndex = static_cast<int>(d / 3);
        }
        std::mt19937_64 layout(37);
        randomizeRoutes(plan, layout, /*poolRate=*/0.15);
        plan.rebuildUnassignedPool();
        DecomposedCalculator calculator;
        defaultConstraints()
                .only({"vehicleCapacityCost", "maxEndTimeCost", "travelTimeCost",
                       "requestGroupCost"})
                .into(calculator);
        optimaru::Editor<PdpRoutePlan> editor(plan, calculator, optimaru::ScoreChecks::Full);
        editor.score();
        optimaru::testing::PreviewParity parity;
        bool corrupted = false;
        try {
            parity = optimaru::testing::run_preview_parity(editor, calculator, /*seed=*/17,
                                                           /*trials=*/3000);
        } catch (const optimaru::ScoreMismatchError& error) {
            corrupted = true;
            std::printf("    %s\n", error.what());
        }
        std::printf("    %s: %d moves checked, %d previewed, %d declined\n",
                    grouped ? "request groups" : "no groups", parity.checked, parity.previewed,
                    parity.declined);
        check(!corrupted && parity.all_equal() && parity.declined == 0 && parity.previewed > 1000,
              std::string(grouped ? "requestGroupCost with groups"
                                  : "requestGroupCost with nothing grouped") +
                      ": every preview served and equal to the applied score (FULL_ASSERT)");
    }
    {
        PdpRoutePlan plan = buildRandomInstance(14, 3, /*seed=*/9);
        plan.rebuildUnassignedPool();
        Solver::Config config;
        config.time_limit = std::chrono::seconds(2);
        config.seed = 3;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruin_recreate_unimproved_step_trigger = 200;
        config.ruin_recreate_polish_steps = 100;
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        bool corrupted = false;
        try {
            Solver solver(config);
            solver.solve(plan, calculator);
        } catch (const optimaru::ScoreMismatchError& e) {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted, "2s FULL_ASSERT solve with repeated, shifted components");
        // plan.score rounds each level on its own (the fractional weights
        // leave fractions), exactly as the solver's levelScore does.
        const auto levels = levelsWith(repeatedAssembly(), plan);
        check(!corrupted && plan.score.hard == levels.hard &&
                      plan.score.medium == optimaru::round_score_level(levels.medium, "medium") &&
                      plan.score.soft == optimaru::round_score_level(levels.soft, "soft"),
              "the reported levels == a fresh recompute of the same assembly");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (m) Negative thresholds. The threshold with an offset < 0 is a fixed
// charge per unit with a non-zero raw cost: it jumps at 0, so a raw cost
// that should be 0 must be exactly 0 on every path. The enclosing radius is
// real-valued (meters); the travel and route times are whole seconds; the
// capacity threshold sits on hard. The calculator previews these instances
// through cost() on the written-out route (never the shared sums) and costs
// every applied route with cost(), so previews, the incremental score and a
// fresh recompute agree; the last on_new_best report (the incremental score of
// the best solution) is plan.score (a fresh recompute of it).
// -------------------------------------------------------------------------
ConstraintAssembly negativeThresholdAssembly() {
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("travelTimeCost", -60.0);
    assembly.offset("vehicleCapacityCost", -0.5);
    assembly.weight("enclosingCircleRadiusCost", 0.01).offset("enclosingCircleRadiusCost", -60.0);
    assembly.weight("routeDurationCost", 0.001).offset("routeDurationCost", -60.0);
    assembly.offset("unassignedRequestsCost", -1.0);
    assembly.add_instances(std::move(copyOf("travelTimeCost")
                                             .weight("travelTimeCost", 1e-4)
                                             .exponent("travelTimeCost", 2.0)
                                             .offset("travelTimeCost", -30.0)));
    return assembly;
}

void verifyNegativeThresholds() {
    std::printf("== (m) Negative thresholds: a fixed charge, computed exactly ==\n");
    {
        // The coupling globals have no preview contract, so the previews are
        // checked on the unit components alone.
        PdpRoutePlan plan = buildRandomInstance(12, 3, /*seed=*/23);
        std::mt19937_64 layout(37);
        randomizeRoutes(plan, layout, /*poolRate=*/0.15);
        plan.rebuildUnassignedPool();
        DecomposedCalculator calculator;
        negativeThresholdAssembly()
                .only({"vehicleCapacityCost", "maxEndTimeCost", "travelTimeCost",
                       "enclosingCircleRadiusCost", "routeDurationCost"})
                .into(calculator);
        optimaru::Editor<PdpRoutePlan> editor(plan, calculator, optimaru::ScoreChecks::Full);
        editor.score();
        optimaru::testing::PreviewParity parity;
        bool corrupted = false;
        try {
            parity = optimaru::testing::run_preview_parity(editor, calculator, /*seed=*/71,
                                                           /*trials=*/3000);
        } catch (const optimaru::ScoreMismatchError& error) {
            corrupted = true;
            std::printf("    %s\n", error.what());
        }
        std::printf("    %d moves checked, %d previewed, %d declined\n", parity.checked,
                    parity.previewed, parity.declined);
        check(!corrupted && parity.all_equal() && parity.declined == 0 && parity.previewed > 1000,
              "negative thresholds: every preview equal to the applied score (FULL_ASSERT)");
    }
    {
        PdpRoutePlan plan = buildRandomInstance(14, 3, /*seed=*/23);
        plan.rebuildUnassignedPool();
        Solver::Config config;
        config.time_limit = std::chrono::seconds(2);
        config.seed = 11;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruin_recreate_unimproved_step_trigger = 200;
        config.ruin_recreate_polish_steps = 100;
        DecomposedCalculator calculator;
        negativeThresholdAssembly().into(calculator);
        bool corrupted = false;
        try {
            Solver(config).solve(plan, calculator);
        } catch (const optimaru::ScoreMismatchError& e) {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted, "2s FULL_ASSERT solve with negative thresholds");
    }
    {
        PdpRoutePlan plan = buildRandomInstance(14, 3, /*seed=*/23);
        plan.rebuildUnassignedPool();
        Solver::Config config;
        config.time_limit = std::chrono::seconds(10);
        config.step_limit = 20000;
        config.seed = 13;
        std::vector<Score> reports;
        config.on_new_best = [&](std::int64_t, const Score& best) { reports.push_back(best); };
        DecomposedCalculator calculator;
        negativeThresholdAssembly().into(calculator);
        Solver(config).solve(plan, calculator);
        const auto levels = levelsWith(negativeThresholdAssembly(), plan);
        check(plan.score.hard == levels.hard &&
                      plan.score.medium == optimaru::round_score_level(levels.medium, "medium") &&
                      plan.score.soft == optimaru::round_score_level(levels.soft, "soft"),
              "20000 steps: plan.score == a fresh recompute of the same assembly");
        check(!reports.empty() && reports.back() == plan.score,
              "the last onNewBest report (incremental) == plan.score (fresh)");
    }
    std::printf("\n");
}
}  // namespace

int main() {
    verifyAssemblyParity();
    verifyIncremental();
    verifyPartialAssemblies();
    verifyOverrides();
    verifyConstraintTotals();
    verifyFullAssertSolve();
    verifyRepairHonoursRequiredVehicle();
    verifyPinnedAndBoundUnderPressure();
    verifyRawCostsByHand();
    verifyCatalog();
    verifyMediumLevel();
    verifyMediumIncremental();
    verifyRepeatsAndOffsets();
    verifyNegativeThresholds();

    if (failures == 0) {
        std::printf("All pickup-delivery plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d pickup-delivery plugin check(s) FAILED.\n", failures);
    return 1;
}

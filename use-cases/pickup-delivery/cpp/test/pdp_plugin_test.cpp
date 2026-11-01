// Plugin-assembly (pdp/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 300 random route
//       layouts (unassigned visits included)
//   (b) incremental score == full recompute across 1500 random LIST moves
//       driven through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: each component isolates its own term
//   (d) weight / level overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(plan, calculator) under FULL_ASSERT
//   (g) the pair move takes a misplaced bound request to its vehicle
//   (h) pinned stops and bound requests hold when other hard rules are broken
//   (i) every component's raw cost by hand: one positive and one zero case
//   (j) the catalog (info()) == each component's kDefaultWeight / kDefaultLevel
//   (k) the medium level: fold, hard() / soft() / weight on
//       unassignedRequestsCost, and incremental == full with those overrides
//   (l) repeated components and the offset / clip knobs: a hand-calculated
//       repeat (travel + 0.5 x travel^2), threshold / literal / negative
//       offsets by hand (an idle vehicle counts in literal mode, the pool
//       only for the two components that price it), the global's total
//       shaped once, incremental == a fresh recompute over a random walk,
//       every preview == the applied score under FULL_ASSERT and a
//       FULL_ASSERT solve with repeated, shifted components
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
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/testing/preview_parity.hpp"

using namespace pdp;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

Score scoreWith(DecomposedCalculator &calculator, PdpRoutePlan &plan)
{
    calculator.resetWorkingSolution(plan);
    return fromEngineScore(calculator.calculateScore());
}

// Spreads every visit over the vehicles at random; `poolRate` of them go to
// the unassigned pool list and another `poolRate` into no list at all.
void randomizeRoutes(PdpRoutePlan &plan, std::mt19937_64 &rng, double poolRate)
{
    for (Vehicle &vehicle : plan.vehicles)
        vehicle.nodeIndices.clear();
    plan.unassignedNodeIndices.clear();
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int v = 0; v < static_cast<int>(plan.nodes.size()); ++v)
    {
        const double roll = unit(rng);
        if (roll < poolRate)
        {
            plan.unassignedNodeIndices.push_back(v);
            continue;
        }
        if (roll < 2 * poolRate)
            continue;  // in no list at all
        plan.vehicles[rng() % plan.vehicles.size()].nodeIndices.push_back(v);
    }
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    PdpRoutePlan plan = buildRandomInstance(20, 4, /*seed=*/11);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    int mismatches = 0;
    const int rounds = 300;
    for (int round = 0; round < rounds; ++round)
    {
        randomizeRoutes(plan, rng, /*poolRate=*/0.10);
        const Score plugin = scoreWith(calculator, plan);
        const Score monolithic = ScoreCalculator::calculateScore(plan);
        if (!(plugin == monolithic))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n",
                            round,
                            plugin.toString().c_str(),
                            monolithic.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(rounds) + " random route layouts score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of LIST
// mutations, driven through the ScoreDirector.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    PdpRoutePlan plan = buildRandomInstance(20, 4, /*seed=*/11);
    plan.rebuildUnassignedPool();

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<PdpRoutePlan> director(plan, calculator);

    using Descriptor = optimaru::SolutionDescriptor<PdpRoutePlan>;
    std::mt19937_64 rng(3);
    int mismatches = 0;
    const int steps = 1500;
    for (int step = 0; step < steps; ++step)
    {
        const int listCount = Descriptor::listCount(plan);
        const int from
            = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));
        if (Descriptor::listSize(plan, from) == 0)
            continue;
        const int position = static_cast<int>(
            rng()
            % static_cast<std::uint64_t>(Descriptor::listSize(plan, from)));
        const int element = Descriptor::getElement(plan, from, position);
        director.removeListElement(from, position);

        const int to
            = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));
        const int insertAt = static_cast<int>(
            rng()
            % static_cast<std::uint64_t>(Descriptor::listSize(plan, to) + 1));
        director.insertListElement(to, insertAt, element);

        const Score incremental = fromEngineScore(director.calculateScore());
        const Score full = ScoreCalculator::calculateScore(plan);
        if (!(incremental == full))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n",
                            step,
                            incremental.toString().c_str(),
                            full.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps)
              + " random list moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Each component in isolation carries exactly its own level.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Single-component assemblies ==\n");
    PdpRoutePlan plan = buildRandomInstance(16, 3, /*seed=*/21);
    std::mt19937_64 rng(5);
    randomizeRoutes(plan, rng, /*poolRate=*/0.15);
    const Score full = ScoreCalculator::calculateScore(plan);

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
void verifyOverrides()
{
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
        check(score.hard == 10 * base.hard,
              "vehicleCapacityCost re-weighted to 10");
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
// (e) constraintTotals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    PdpRoutePlan plan = buildRandomInstance(14, 3, /*seed=*/9);
    std::mt19937_64 rng(5);
    randomizeRoutes(plan, rng, /*poolRate=*/0.10);

    const std::size_t componentCount = defaultConstraints().size();
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(plan);
    const optimaru::HardSoftScore score = calculator.calculateScore();

    // A medium total is in medium units (not multiplied by the fold), so it
    // is folded here the way the calculator folds its medium accumulator.
    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
    {
        if (total.level == optimaru::Level::Hard)
            hard += total.cost;
        else if (total.level == optimaru::Level::Medium)
            soft += total.cost * static_cast<double>(kMediumUnit);
        else
            soft += total.cost;
    }
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-3,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == componentCount,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    PdpRoutePlan plan = buildRandomInstance(14, 3, /*seed=*/9);
    plan.rebuildUnassignedPool();

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    bool corrupted = false;
    try
    {
        Solver solver(config);
        solver.solve(plan, calculator);
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(plan);
        check(plan.score == recomputed,
              "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) The pair move takes a misplaced bound request straight to its vehicle.
// -------------------------------------------------------------------------
void verifyRepairHonoursRequiredVehicle()
{
    std::printf("== (g) Repair and requiredVehicleCost ==\n");
    // A single request, so no stop is routed near it and the pair move falls
    // back on picking a vehicle. With only that move and a handful of steps, a
    // pick among all the vehicles would almost never find the right one.
    const int vehicleCount = 40;
    int repaired = 0;
    const int seeds = 5;
    for (int seed = 0; seed < seeds; ++seed)
    {
        PdpRoutePlan plan = buildRandomInstance(1, vehicleCount, /*seed=*/21);
        plan.deliveries[0].requiredVehicleIndex = vehicleCount - 1;
        plan.vehicles[0].nodeIndices = {plan.deliveries[0].pickupNodeIndex,
                                        plan.deliveries[0].dropoffNodeIndex};

        Solver::Config config;
        config.stepCountLimit = 8;
        config.spentLimit = std::chrono::minutes(1);  // safety net only
        config.randomSeed = static_cast<std::uint64_t>(seed);
        config.pairAssignmentProbability = 0.0;
        config.relocateMoveWeight = 0.0;
        config.listSwapMoveWeight = 0.0;
        config.twoOptMoveWeight = 0.0;
        config.orOptMoveWeight = 0.0;
        config.tailSwapMoveWeight = 0.0;
        config.ruinRecreate = false;

        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        Solver(config).solve(plan, calculator);
        if (plan.vehicles[vehicleCount - 1].nodeIndices.size() == 2)
            ++repaired;
    }
    check(repaired == seeds, "a misplaced bound request reaches its vehicle");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) The score keeps pinned stops and bound requests where they belong, even
// when something else is already broken.
// -------------------------------------------------------------------------
Solver::Config stepLimited(std::uint64_t seed)
{
    Solver::Config config;
    config.stepCountLimit = 20000;
    config.spentLimit = std::chrono::minutes(1);  // safety net only
    config.randomSeed = seed;
    return config;
}

void verifyPinnedAndBoundUnderPressure()
{
    std::printf("== (h) Pinned stops and bound requests under pressure ==\n");
    int pinnedHeld = 0;
    int stayedOff = 0;
    const int seeds = 5;
    for (int seed = 0; seed < seeds; ++seed)
    {
        // Both pickups are pinned, the second one days late behind the first.
        // Swapping them would save far more lateness than it used to cost.
        PdpRoutePlan late = buildRandomInstance(2, 1, /*seed=*/23);
        const int first = late.deliveries[0].pickupNodeIndex;
        const int second = late.deliveries[1].pickupNodeIndex;
        for (int nodeIndex : {first, late.deliveries[0].dropoffNodeIndex})
        {
            late.nodes[nodeIndex].minStartTime = 500000;
            late.nodes[nodeIndex].maxEndTime = 600000;
        }
        for (int nodeIndex : {second, late.deliveries[1].dropoffNodeIndex})
        {
            late.nodes[nodeIndex].minStartTime = 0;
            late.nodes[nodeIndex].maxEndTime = 40000;
        }
        late.vehicles[0].nodeIndices = {first,
                                        second,
                                        late.deliveries[0].dropoffNodeIndex,
                                        late.deliveries[1].dropoffNodeIndex};
        late.vehicles[0].pinnedNodeIndices = {first, second};
        DecomposedCalculator lateCalculator;
        addDefaultConstraints(lateCalculator);
        Solver(stepLimited(seed)).solve(late, lateCalculator);
        const std::vector<int> &route = late.vehicles[0].nodeIndices;
        if (route.size() >= 2 && route[0] == first && route[1] == second)
            ++pinnedHeld;

        // Vehicle 1 cannot reach the request in time and vehicle 0 is not
        // allowed to take it.
        PdpRoutePlan bound = buildRandomInstance(1, 2, /*seed=*/23);
        bound.deliveries[0].requiredVehicleIndex = 1;
        for (Seconds &seconds : bound.drivingSeconds[1])
            seconds = 10000000;
        DecomposedCalculator boundCalculator;
        addDefaultConstraints(boundCalculator);
        Solver(stepLimited(seed)).solve(bound, boundCalculator);
        if (bound.vehicles[0].nodeIndices.empty())
            ++stayedOff;
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
PdpRoutePlan buildHandInstance()
{
    PdpRoutePlan plan;
    plan.name = "hand";
    for (int k = 0; k < 5; ++k)
        plan.locations.push_back(Location{0.0, 0.001 * k, k});
    for (int v = 0; v < 2; ++v)
    {
        Vehicle vehicle;
        vehicle.id = std::to_string(v);
        vehicle.capacity = 10;
        vehicle.homeLocationIndex = 0;
        plan.vehicles.push_back(vehicle);
    }
    const char *ids[] = {"p1", "d1", "p2", "d2"};
    for (int n = 0; n < 4; ++n)
        plan.nodes.push_back(Node{ids[n], n + 1, 0, 24 * 3600, 60, n / 2,
                                  n % 2 == 0, -1, NULL_TIME});
    plan.deliveries = {Delivery{"r1", 0, 1, 4, 0, -1},
                       Delivery{"r2", 2, 3, 4, 0, -1}};
    plan.drivingSeconds.assign(5, std::vector<Seconds>(5, 0));
    for (int a = 0; a < 5; ++a)
        for (int b = 0; b < 5; ++b)
            plan.drivingSeconds[a][b] = 100 * std::abs(a - b);
    return plan;
}

// Vehicle 0 serves p1 d1 p2 d2; vehicle 1 and the pool are empty.
PdpRoutePlan handRouted()
{
    PdpRoutePlan plan = buildHandInstance();
    plan.vehicles[0].nodeIndices = {0, 1, 2, 3};
    return plan;
}

// The raw per-unit costs (vehicle 0, vehicle 1, pool; one entry for a global)
// of one registry name, read off the default assembly.
std::vector<double> rawCosts(PdpRoutePlan plan, const std::string &name)
{
    plan.rebuildUnassignedPool();
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(plan);
    calculator.calculateScore();
    for (const auto &entry : calculator.constraintCosts())
        if (entry.name == name)
            return entry.unitCosts;
    throw std::logic_error("no constraint " + name);
}

bool near(const std::vector<double> &got, const std::vector<double> &want)
{
    if (got.size() != want.size())
        return false;
    for (std::size_t i = 0; i < got.size(); ++i)
        if (std::fabs(got[i] - want[i]) > 1e-6 * std::max(1.0, std::fabs(want[i])))
            return false;
    return true;
}

void byHand(const std::string &name,
            const std::function<void(PdpRoutePlan &)> &positive,
            const std::vector<double> &positiveCosts,
            const std::function<void(PdpRoutePlan &)> &zero,
            const std::vector<double> &zeroCosts)
{
    PdpRoutePlan up = handRouted();
    positive(up);
    check(near(rawCosts(up, name), positiveCosts), name + " positive");
    PdpRoutePlan down = handRouted();
    zero(down);
    check(near(rawCosts(down, name), zeroCosts), name + " zero");
}

void verifyRawCostsByHand()
{
    std::printf("== (i) Raw costs by hand ==\n");
    constexpr double pi = 3.14159265358979323846;
    // On the equator the haversine distance is R * dLongitude (radians), and
    // the equirectangular projection keeps the stops on one line.
    const double meterPerStep = 6371000.0 * 0.001 * pi / 180.0;
    const std::vector<double> none{0.0, 0.0, 0.0};
    const std::vector<double> noGlobal{0.0};
    const auto keep = [](PdpRoutePlan &) {};

    // Capacity 3: the load reaches 4 after p1 and after p2 -> 1 + 1.
    byHand("vehicleCapacityCost",
           [](PdpRoutePlan &p) { p.vehicles[0].capacity = 3; }, {2.0, 0.0, 0.0},
           keep, none);
    // p2 finishes at 480 against 400 (80 s -> 2 min), d2 at 640 against 580
    // (60 s -> 1 min).
    byHand("maxEndTimeCost",
           [](PdpRoutePlan &p)
           {
               p.nodes[2].maxEndTime = 400;
               p.nodes[3].maxEndTime = 580;
           },
           {3.0, 0.0, 0.0},
           [](PdpRoutePlan &p) { p.nodes[3].maxEndTime = 640; }, none);
    // 4 legs of 100 s, open; returnToDepot adds d2 -> home = 400 s.
    byHand("travelTimeCost", keep, {400.0, 0.0, 0.0},
           [](PdpRoutePlan &p) { p.vehicles[0].nodeIndices.clear(); }, none);
    byHand("travelTimeCost",
           [](PdpRoutePlan &p) { p.vehicles[0].returnToDepot = true; },
           {800.0, 0.0, 0.0},
           [](PdpRoutePlan &p)
           {
               p.vehicles[0].returnToDepot = true;
               p.vehicles[0].nodeIndices.clear();  // a parked vehicle drives 0
           },
           none);
    byHand("maxTravelTimeCost",
           [](PdpRoutePlan &p) { p.vehicles[0].maxTravelTime = 300; },
           {100.0, 0.0, 0.0},
           [](PdpRoutePlan &p) { p.vehicles[0].maxTravelTime = 400; }, none);
    byHand("maxRouteDurationCost",
           [](PdpRoutePlan &p) { p.vehicles[0].maxRouteDuration = 600; },
           {40.0, 0.0, 0.0},
           [](PdpRoutePlan &p) { p.vehicles[0].maxRouteDuration = 640; }, none);
    byHand("vehicleMaxEndTimeCost",
           [](PdpRoutePlan &p) { p.vehicles[0].maxEndTime = 600; },
           {40.0, 0.0, 0.0},
           [](PdpRoutePlan &p) { p.vehicles[0].maxEndTime = 640; }, none);
    byHand("maxStopsCost", [](PdpRoutePlan &p) { p.vehicles[0].maxStops = 3; },
           {1.0, 0.0, 0.0},
           [](PdpRoutePlan &p) { p.vehicles[0].maxStops = 4; }, none);
    // The farthest stop (d2) is 4 steps from home.
    byHand("maxRouteRadiusCost",
           [&](PdpRoutePlan &p)
           { p.vehicles[0].maxRouteRadiusMeters = 4 * meterPerStep - 25.0; },
           {25.0, 0.0, 0.0},
           [&](PdpRoutePlan &p)
           { p.vehicles[0].maxRouteRadiusMeters = 4 * meterPerStep + 1.0; },
           none);
    // Four stops on one line, 3 steps from the first to the last: radius 1.5.
    byHand("enclosingCircleRadiusCost", keep, {1.5 * meterPerStep, 0.0, 0.0},
           [](PdpRoutePlan &p) { p.vehicles[0].nodeIndices = {0}; }, none);
    // Target 10 * 1.0 against 4 + 4 delivered.
    byHand("minCapacityUtilizationCost",
           [](PdpRoutePlan &p) { p.vehicles[0].minLoadRatio = 1.0; },
           {2.0, 0.0, 0.0},
           [](PdpRoutePlan &p) { p.vehicles[0].minLoadRatio = 0.5; }, none);
    byHand("routeDurationCost", keep, {640.0, 0.0, 0.0},
           [](PdpRoutePlan &p) { p.vehicles[0].nodeIndices.clear(); }, none);
    // 100 + 260 + 420 + 580.
    byHand("latencyCost", keep, {1360.0, 0.0, 0.0},
           [](PdpRoutePlan &p) { p.vehicles[0].nodeIndices.clear(); }, none);
    // d1 finishes at 320, d2 at 640, both ordered at 0: 320^2 + 640^2. Zero:
    // each request ordered exactly when its dropoff finishes.
    byHand("squaredOrderAgeCost", keep, {512000.0, 0.0, 0.0},
           [](PdpRoutePlan &p)
           {
               p.deliveries[0].orderTime = 320;
               p.deliveries[1].orderTime = 640;
           },
           none);
    // In the pool every dropoff is charged as finishing at 3600: 2 * 3600^2.
    byHand("squaredOrderAgeCost",
           [](PdpRoutePlan &p) { p.vehicles[0].nodeIndices.clear(); },
           {0.0, 0.0, 25920000.0},
           [](PdpRoutePlan &p)
           {
               p.vehicles[0].nodeIndices.clear();
               p.deliveries[0].orderTime = 3600;
               p.deliveries[1].orderTime = 3600;
           },
           none);
    byHand("vehicleFixedCost", [](PdpRoutePlan &p) { p.vehicles[0].fixedCost = 50.0; },
           {50.0, 0.0, 0.0},
           [](PdpRoutePlan &p)
           {
               p.vehicles[1].fixedCost = 50.0;  // vehicle 1 stays unused
           },
           none);
    // Pinned d1, p1 against a route starting p1, d1: both positions wrong.
    byHand("pinnedStopsCost",
           [](PdpRoutePlan &p) { p.vehicles[0].pinnedNodeIndices = {1, 0}; },
           {2.0, 0.0, 0.0},
           [](PdpRoutePlan &p) { p.vehicles[0].pinnedNodeIndices = {0, 1}; },
           none);
    // r1 bound to vehicle 1 rides vehicle 0 (2); r2 bound to vehicle 1 waits
    // in the pool (1).
    byHand("requiredVehicleCost",
           [](PdpRoutePlan &p)
           {
               p.deliveries[0].requiredVehicleIndex = 1;
               p.deliveries[1].requiredVehicleIndex = 1;
               p.vehicles[0].nodeIndices = {0, 1};
           },
           {2.0, 0.0, 1.0},
           [](PdpRoutePlan &p) { p.deliveries[0].requiredVehicleIndex = 0; },
           none);
    // d2 left out of the route: r2 is half-routed.
    byHand("pairedRequestCost",
           [](PdpRoutePlan &p) { p.vehicles[0].nodeIndices = {0, 1, 2}; }, {1.0},
           keep, noGlobal);
    // d2 rides vehicle 1.
    byHand("sameVehicleCost",
           [](PdpRoutePlan &p)
           {
               p.vehicles[0].nodeIndices = {0, 1, 2};
               p.vehicles[1].nodeIndices = {3};
           },
           {1.0}, keep, noGlobal);
    byHand("pickupBeforeDropoffCost",
           [](PdpRoutePlan &p) { p.vehicles[0].nodeIndices = {1, 0, 2, 3}; },
           {1.0}, keep, noGlobal);
    // Both requests unrouted: (60 + 60) / 60 = 2 minutes each.
    byHand("unassignedRequestsCost",
           [](PdpRoutePlan &p) { p.vehicles[0].nodeIndices.clear(); }, {4.0},
           keep, noGlobal);
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) The catalog read off the default assembly == the components' constants.
// -------------------------------------------------------------------------
template <typename Component>
bool rowIs(const optimaru::ConstraintInfo &row,
           const char *name,
           optimaru::Level level,
           double weight,
           bool isGlobal)
{
    return row.name == name && row.defaultLevel == Component::kDefaultLevel
           && row.defaultWeight == Component::kDefaultWeight
           && row.defaultLevel == level && row.defaultWeight == weight
           && row.levelFlippable && !row.reward && row.isGlobal == isGlobal
           && row.defaultEnabled == (weight > 0.0);
}

void verifyCatalog()
{
    std::printf("== (j) Catalog == component constants ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 20, "20 catalog rows");
    if (rows.size() != 20)
        return;
    // Unit constraints first, then the globals, in emplace order: the
    // list_constraints() order.
    check(rowIs<VehicleCapacityCost>(rows[0], "vehicleCapacityCost", Level::Hard, 1.0, false)
              && rowIs<MaxEndTimeCost>(rows[1], "maxEndTimeCost", Level::Hard, 1.0, false)
              && rowIs<TravelTimeCost>(rows[2], "travelTimeCost", Level::Soft, 1.0, false)
              && rowIs<MaxTravelTimeCost>(rows[3], "maxTravelTimeCost", Level::Hard, 1.0, false)
              && rowIs<MaxRouteDurationCost>(rows[4], "maxRouteDurationCost", Level::Hard, 1.0, false)
              && rowIs<VehicleMaxEndTimeCost>(rows[5], "vehicleMaxEndTimeCost", Level::Hard, 1.0, false)
              && rowIs<MaxStopsCost>(rows[6], "maxStopsCost", Level::Hard, 1.0, false)
              && rowIs<MaxRouteRadiusCost>(rows[7], "maxRouteRadiusCost", Level::Hard, 1.0, false)
              && rowIs<EnclosingCircleRadiusCost>(rows[8], "enclosingCircleRadiusCost", Level::Soft, 0.0, false)
              && rowIs<MinCapacityUtilizationCost>(rows[9], "minCapacityUtilizationCost", Level::Soft, 1.0, false)
              && rowIs<RouteDurationCost>(rows[10], "routeDurationCost", Level::Soft, 0.0, false)
              && rowIs<LatencyCost>(rows[11], "latencyCost", Level::Soft, 0.0, false)
              && rowIs<SquaredOrderAgeCost>(rows[12], "squaredOrderAgeCost", Level::Soft, 0.0, false)
              && rowIs<VehicleFixedCost>(rows[13], "vehicleFixedCost", Level::Soft, 1.0, false)
              && rowIs<PinnedStopsCost>(rows[14], "pinnedStopsCost", Level::Hard, 1e12, false)
              && rowIs<RequiredVehicleCost>(rows[15], "requiredVehicleCost", Level::Hard, 1000.0, false)
              && rowIs<PairedRequestCost>(rows[16], "pairedRequestCost", Level::Hard, 1000.0, true)
              && rowIs<SameVehicleCost>(rows[17], "sameVehicleCost", Level::Hard, 1000.0, true)
              && rowIs<PickupBeforeDropoffCost>(rows[18], "pickupBeforeDropoffCost", Level::Hard, 1000.0, true)
              && rowIs<UnassignedRequestsCost>(rows[19], "unassignedRequestsCost", Level::Medium, 1.0, true),
          "info() == the components' kDefaultWeight / kDefaultLevel");
    check(PairedRequestCost::kDefaultWeight == ScoreCalculator::kCouplingPenalty
              && RequiredVehicleCost::kDefaultWeight == ScoreCalculator::kCouplingPenalty
              && PinnedStopsCost::kDefaultWeight == ScoreCalculator::kPinnedPenalty,
          "the penalty defaults are ScoreCalculator's constants");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (k) The medium level: unassignedRequestsCost.
// -------------------------------------------------------------------------
optimaru::DecomposedScoreCalculator<PdpRoutePlan>::LevelScores
levelsWith(ConstraintAssembly assembly, PdpRoutePlan &plan)
{
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    calculator.resetWorkingSolution(plan);
    calculator.calculateScore();
    return calculator.levelScores();
}

void verifyMediumLevel()
{
    std::printf("== (k) Medium level ==\n");
    // Vehicle 0 serves r1; r2 is dropped: 2 medium minutes, 200 s of driving.
    PdpRoutePlan plan = buildHandInstance();
    plan.vehicles[0].nodeIndices = {0, 1};
    plan.rebuildUnassignedPool();
    const Score monolithic = ScoreCalculator::calculateScore(plan);
    check(monolithic.hard == 0 && monolithic.medium == -2 && monolithic.soft == -200,
          "the hand plan drops 2 medium minutes");

    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        calculator.resetWorkingSolution(plan);
        const optimaru::HardSoftScore engine = calculator.calculateScore();
        check(engine.hard == 0
                  && engine.soft == -200.0 - 2.0 * static_cast<double>(kMediumUnit),
              "default: medium folded into soft x 10^9");
        check(engine.hard == toEngineScore(monolithic).hard
                  && engine.soft == toEngineScore(monolithic).soft,
              "default: the fold equals the monolithic Score adapter");
        const auto levels = calculator.levelScores();
        check(levels.hard == 0 && levels.medium == -2.0 && levels.soft == -200.0,
              "levelScores() reports the unfolded levels");
    }
    {
        const auto levels
            = levelsWith(defaultConstraints().hard("unassignedRequestsCost"), plan);
        check(levels.hard == -2 && levels.medium == 0.0 && levels.soft == -200.0,
              "hard(): one hard unit per raw minute");
    }
    {
        const auto levels
            = levelsWith(defaultConstraints().soft("unassignedRequestsCost"), plan);
        check(levels.hard == 0 && levels.medium == 0.0 && levels.soft == -202.0,
              "soft(): one soft unit per raw minute, outside the fold");
    }
    {
        const auto levels = levelsWith(
            defaultConstraints().weight("unassignedRequestsCost", 3), plan);
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
// ScoreDirector protocol (the coupling globals have no preview contract, so
// the engine applies and scores every move; this walk is that path).
void verifyMediumIncremental()
{
    std::printf("== (k) Medium overrides: incremental == full ==\n");
    const std::vector<std::pair<std::string, std::function<ConstraintAssembly()>>>
        variants{
            {"default", [] { return defaultConstraints(); }},
            {"hard", [] { return defaultConstraints().hard("unassignedRequestsCost"); }},
            {"soft", [] { return defaultConstraints().soft("unassignedRequestsCost"); }},
            {"weight 3",
             [] { return defaultConstraints().weight("unassignedRequestsCost", 3); }},
            {"with travelTimeCost on medium",
             [] {
                 return defaultConstraints().level("travelTimeCost", optimaru::Level::Medium);
             }},
        };
    using Descriptor = optimaru::SolutionDescriptor<PdpRoutePlan>;
    for (const auto &variant : variants)
    {
        PdpRoutePlan plan = buildRandomInstance(12, 3, /*seed=*/17);
        plan.rebuildUnassignedPool();
        DecomposedCalculator calculator;
        variant.second().into(calculator);
        optimaru::ScoreDirector<PdpRoutePlan> director(plan, calculator);
        std::mt19937_64 rng(41);
        int mismatches = 0;
        int mediumSteps = 0;
        for (int step = 0; step < 600; ++step)
        {
            const int listCount = Descriptor::listCount(plan);
            const int from = static_cast<int>(
                rng() % static_cast<std::uint64_t>(listCount));
            if (Descriptor::listSize(plan, from) == 0)
                continue;
            const int position = static_cast<int>(
                rng()
                % static_cast<std::uint64_t>(Descriptor::listSize(plan, from)));
            const int element = Descriptor::getElement(plan, from, position);
            director.removeListElement(from, position);
            const int to = static_cast<int>(
                rng() % static_cast<std::uint64_t>(listCount));
            const int insertAt = static_cast<int>(
                rng()
                % static_cast<std::uint64_t>(Descriptor::listSize(plan, to) + 1));
            director.insertListElement(to, insertAt, element);

            const optimaru::HardSoftScore incremental = director.calculateScore();
            DecomposedCalculator fresh;
            variant.second().into(fresh);
            fresh.resetWorkingSolution(plan);
            const optimaru::HardSoftScore full = fresh.calculateScore();
            if (fresh.levelScores().medium != 0.0
                || calculator.levelScores().medium != 0.0)
                ++mediumSteps;
            if (incremental.hard != full.hard
                || std::fabs(incremental.soft - full.soft) > 1e-6)
                ++mismatches;
        }
        check(mismatches == 0,
              "unassignedRequestsCost " + variant.first
                  + ": 600 random list moves, incremental == full recompute");
        if (variant.first != "hard" && variant.first != "soft")
            check(mediumSteps > 0, variant.first + ": the walk visits medium states");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (l) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with addInstances(), as the Python bindings
// build it from a pick list.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string &name)
{
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
PdpRoutePlan repeatInstance()
{
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

DecomposedCalculator::LevelScores levelsOf(ConstraintAssembly assembly, PdpRoutePlan plan)
{
    return levelsWith(std::move(assembly), plan);
}

// Every shaped flavour at once: unit constraints evaluating the vehicles
// (travel, capacity, lateness, latency), one evaluating the pool too
// (squaredOrderAgeCost), the medium global and a coupling global.
ConstraintAssembly repeatedAssembly()
{
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("travelTimeCost", 3000.0).clip("travelTimeCost", false);
    assembly.offset("unassignedRequestsCost", 1.0);
    assembly.addInstances(std::move(copyOf("travelTimeCost")
                                        .weight("travelTimeCost", 0.001)
                                        .exponent("travelTimeCost", 2.0)
                                        .offset("travelTimeCost", 2000.0)));
    assembly.addInstances(std::move(copyOf("vehicleCapacityCost")
                                        .weight("vehicleCapacityCost", 2.0)
                                        .offset("vehicleCapacityCost", 1.0)
                                        .clip("vehicleCapacityCost", false)));
    assembly.addInstances(std::move(copyOf("unassignedRequestsCost")
                                        .weight("unassignedRequestsCost", 2.0)
                                        .offset("unassignedRequestsCost", 3.0)
                                        .clip("unassignedRequestsCost", false)));
    assembly.addInstances(std::move(copyOf("maxEndTimeCost").offset("maxEndTimeCost", -2.0)));
    assembly.addInstances(std::move(copyOf("latencyCost")
                                        .weight("latencyCost", 0.01)
                                        .offset("latencyCost", 5000.0)
                                        .clip("latencyCost", false)));
    assembly.addInstances(std::move(copyOf("squaredOrderAgeCost")
                                        .weight("squaredOrderAgeCost", 1e-6)
                                        .offset("squaredOrderAgeCost", 1e6)
                                        .clip("squaredOrderAgeCost", false)));
    assembly.addInstances(std::move(
        copyOf("pairedRequestCost").offset("pairedRequestCost", 1.0).clip("pairedRequestCost", false)));
    return assembly;
}

void verifyRepeatsAndOffsets()
{
    std::printf("== (l) Repeated components, offset / clip ==\n");
    {
        PdpRoutePlan plan = repeatInstance();
        ConstraintAssembly assembly = copyOf("travelTimeCost");
        assembly.addInstances(std::move(copyOf("travelTimeCost")
                                            .weight("travelTimeCost", 0.5)
                                            .exponent("travelTimeCost", 2.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        calculator.resetWorkingSolution(plan);
        const optimaru::HardSoftScore score = calculator.calculateScore();
        check(score.hard == 0 && score.soft == -(600.0 + 0.5 * (200.0 * 200.0 + 400.0 * 400.0)),
              "travel + 0.5 x travel^2 per vehicle: -(600 + 100000) soft");
        const auto entries = calculator.constraintCosts();
        check(entries.size() == 2 && entries[0].unitCosts == entries[1].unitCosts
                  && entries[0].unitCosts == std::vector<double>{200.0, 400.0, 0.0, 0.0}
                  && entries[1].weight == 0.5 && entries[1].exponent == 2.0
                  && entries[1].defaultWeight == 1.0,
              "two instances, the same raw costs, each with its own knobs");
        check(entries.size() == 2 && entries[0].scoped && entries[1].scoped
                  && entries[0].units == std::vector<int>{0, 1, 2}
                  && entries[1].units == std::vector<int>{0, 1, 2},
              "a per-vehicle component evaluates the three vehicles, not the pool");
    }
    {
        check(levelsOf(copyOf("travelTimeCost").offset("travelTimeCost", 300.0), repeatInstance())
                      .soft
                  == -100.0,
              "threshold offset 300: 100");
        // Literal: (200 - 300) + (400 - 300) + (0 - 300) = -300, the idle
        // vehicle included and the pool not (it would make -600).
        check(levelsOf(copyOf("travelTimeCost")
                           .offset("travelTimeCost", 300.0)
                           .clip("travelTimeCost", false),
                       repeatInstance())
                      .soft
                  == 300.0,
              "literal offset 300: -100 + 100 - 300 -> +300 soft, the pool not charged");
        check(levelsOf(copyOf("travelTimeCost").offset("travelTimeCost", -50.0), repeatInstance())
                      .soft
                  == -700.0,
              "threshold offset -50: 250 + 450, the idle vehicle stays 0");
        check(levelsOf(copyOf("travelTimeCost")
                           .offset("travelTimeCost", -50.0)
                           .clip("travelTimeCost", false),
                       repeatInstance())
                      .soft
                  == -750.0,
              "literal offset -50: 250 + 450 + 50");
        // A hard literal pick rounds per vehicle: (2 - 2.5) -> -1, (0 - 2.5)
        // -> -3 twice: -7 (a rounded total would give -6).
        check(levelsOf(copyOf("vehicleCapacityCost")
                           .offset("vehicleCapacityCost", 2.5)
                           .clip("vehicleCapacityCost", false),
                       repeatInstance())
                      .hard
                  == 7,
              "capacity literal offset 2.5: llround per vehicle, +7 hard");
        // requiredVehicleCost prices the pool: r3 bound to vehicle 0 and
        // waiting there costs 1 on the pool's unit, and a literal offset 2
        // charges all four units: 3 x (0 - 2) + (1 - 2) = -7.
        PdpRoutePlan bound = repeatInstance();
        bound.deliveries[2].requiredVehicleIndex = 0;
        DecomposedCalculator required;
        copyOf("requiredVehicleCost")
            .weight("requiredVehicleCost", 1.0)
            .offset("requiredVehicleCost", 2.0)
            .clip("requiredVehicleCost", false)
            .into(required);
        required.resetWorkingSolution(bound);
        check(required.calculateScore().hard == 7 && !required.constraintCosts()[0].scoped
                  && required.constraintCosts()[0].unitCosts
                         == std::vector<double>{0.0, 0.0, 0.0, 1.0},
              "requiredVehicleCost evaluates the pool too: literal offset 2 -> +7 hard");
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
        const auto twice
            = levelsOf(std::move(copyOf("unassignedRequestsCost")
                                     .addInstances(std::move(copyOf("unassignedRequestsCost")
                                                                 .weight("unassignedRequestsCost",
                                                                         2.0)))),
                       repeatInstance());
        check(twice.medium == -12.0, "a repeated medium pick: -4 - 8 medium");
    }
    using Descriptor = optimaru::SolutionDescriptor<PdpRoutePlan>;
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        PdpRoutePlan plan = buildRandomInstance(12, 3, /*seed=*/17);
        std::mt19937_64 layout(29);
        randomizeRoutes(plan, layout, /*poolRate=*/0.15);
        plan.rebuildUnassignedPool();
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::ScoreDirector<PdpRoutePlan> director(plan, calculator);
        std::mt19937_64 rng(43);
        int mismatches = 0;
        double drift = 0.0;
        const int steps = 1500;
        for (int step = 0; step < steps; ++step)
        {
            const int listCount = Descriptor::listCount(plan);
            const int from
                = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));
            if (Descriptor::listSize(plan, from) == 0)
                continue;
            const int position = static_cast<int>(
                rng() % static_cast<std::uint64_t>(Descriptor::listSize(plan, from)));
            const int element = Descriptor::getElement(plan, from, position);
            director.removeListElement(from, position);
            const int to = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));
            const int insertAt = static_cast<int>(
                rng() % static_cast<std::uint64_t>(Descriptor::listSize(plan, to) + 1));
            director.insertListElement(to, insertAt, element);

            const optimaru::HardSoftScore incremental = director.calculateScore();
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            fresh.resetWorkingSolution(plan);
            const optimaru::HardSoftScore full = fresh.calculateScore();
            drift = std::max(drift, std::fabs(full.soft - incremental.soft));
            if (!optimaru::scoresAgree(incremental, full))
                ++mismatches;
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0,
              std::to_string(steps)
                  + " random list moves with repeated, shifted components: incremental == fresh");
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
        optimaru::ScoreDirector<PdpRoutePlan> director(plan, calculator,
                                                       optimaru::EnvironmentMode::FULL_ASSERT);
        director.calculateScore();
        optimaru::testing::PreviewParity parity;
        bool corrupted = false;
        try
        {
            parity = optimaru::testing::runPreviewParity(director, calculator, /*seed=*/13,
                                                         /*trials=*/3000);
        }
        catch (const optimaru::ScoreCorruptionError &error)
        {
            corrupted = true;
            std::printf("    %s\n", error.what());
        }
        std::printf("    %d moves checked, %d previewed, %d declined\n", parity.checked,
                    parity.previewed, parity.declined);
        check(!corrupted && parity.allEqual() && parity.declined == 0 && parity.previewed > 1000,
              "repeated, shifted unit components: every preview served and equal to the "
              "applied score (FULL_ASSERT)");
    }
    {
        PdpRoutePlan plan = buildRandomInstance(14, 3, /*seed=*/9);
        plan.rebuildUnassignedPool();
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(2);
        config.randomSeed = 3;
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        config.ruinRecreateUnimprovedStepTrigger = 200;
        config.ruinRecreatePolishSteps = 100;
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        bool corrupted = false;
        try
        {
            Solver solver(config);
            solver.solve(plan, calculator);
        }
        catch (const optimaru::ScoreCorruptionError &e)
        {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted, "2s FULL_ASSERT solve with repeated, shifted components");
        // plan.score rounds each level on its own (the fractional weights
        // leave fractions), exactly as the solver's levelScore does.
        const auto levels = levelsWith(repeatedAssembly(), plan);
        check(!corrupted && plan.score.hard == levels.hard
                  && plan.score.medium == optimaru::roundScoreLevel(levels.medium, "medium")
                  && plan.score.soft == optimaru::roundScoreLevel(levels.soft, "soft"),
              "the reported levels == a fresh recompute of the same assembly");
    }
    std::printf("\n");
}
}  // namespace

int main()
{
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

    if (failures == 0)
    {
        std::printf("All pickup-delivery plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d pickup-delivery plugin check(s) FAILED.\n", failures);
    return 1;
}

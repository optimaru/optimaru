// Vehicle-routing tests: the Java ConstraintProviderTest fixtures, the
// incremental calculator against the full recompute, and the engine
// integration (element conservation, unassigned pool, ALNS).
#include <chrono>
#include <algorithm>
#include <random>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/config/localsearch/local_search_config.hpp"
#include "optimaru/impl/heuristic/selector/common/nearby/nearby_neighborhoods.hpp"
#include "optimaru/impl/localsearch/element_index.hpp"
#include "optimaru/impl/localsearch/greedy_repair.hpp"
#include "optimaru/testing/preview_parity.hpp"
#include "vrp/demo_data.hpp"
#include "vrp/domain.hpp"
#include "vrp/haversine.hpp"
#include "vrp/incremental_calculator.hpp"
#include "vrp/score_calculator.hpp"
#include "vrp/solver.hpp"

using namespace vrp;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

void checkEqual(const std::string &label, long actual, long expected)
{
    const bool ok = (actual == expected);
    std::printf("  [%s] %-46s got=%ld expected=%ld\n", ok ? "PASS" : "FAIL",
                label.c_str(), actual, expected);
    if (!ok)
        ++failures;
}

// -------------------------------------------------------------------------
// The exact fixtures from the Java VehicleRoutingConstraintProviderTest.
// -------------------------------------------------------------------------
void testJavaFixtures()
{
    std::printf("== Scoring against the Java ConstraintProviderTest ==\n");

    VehicleRoutePlan plan = buildJavaFixturePlan();
    const int home = 0, loc2 = 1, loc3 = 2;

    // Driving times documented in the Java test's comments.
    checkEqual("drivingTime L1->L2", plan.drivingTime(home, loc2), 843);
    checkEqual("drivingTime L2->L3", plan.drivingTime(loc2, loc3), 639);
    checkEqual("drivingTime L1->L3", plan.drivingTime(home, loc3), 941);

    // vehicleCapacity: capacity 100, demands 80 + 40 -> 20 over.
    plan.vehicles = {Vehicle{"1", 100, home, hourMinute(7, 0), {}}};
    plan.visits = {
        Visit{"2", "John", loc2, 80, hourMinute(8, 0), hourMinute(10, 0), 30 * 60,
              -1, NULL_TIME},
        Visit{"3", "Paul", loc3, 40, hourMinute(8, 0), hourMinute(10, 0), 30 * 60,
              -1, NULL_TIME},
    };
    plan.vehicles[0].visitIndices = {0, 1};
    Score s = ScoreCalculator::calculateScore(plan);
    checkEqual("vehicleCapacity hard penalty", -s.hard, 20);
    // minimizeTravelTime: the route is open by default, so only
    // home->L2->L3 = 843 + 639 = 1482 is driven.
    checkEqual("minimizeTravelTime soft penalty (open route)", -s.soft, 1482);
    // With returnToDepot the Java round trip home->L2->L3->home is charged:
    // 843 + 639 + 941 = 2423.
    plan.vehicles[0].returnToDepot = true;
    s = ScoreCalculator::calculateScore(plan);
    checkEqual("minimizeTravelTime soft penalty (return to depot)", -s.soft, 2423);
    plan.vehicles[0].returnToDepot = false;

    // maximizeVisitsAssigned: one unassigned visit with a 30-minute service
    // duration is penalized by 30 on the medium level.
    VehicleRoutePlan plan2;
    addLocation(plan2, 49.288087, 16.562172);
    addLocation(plan2, 49.190922, 16.624466);
    HaversineDrivingTimeCalculator::initDrivingTimeMatrix(plan2);
    plan2.vehicles = {Vehicle{"1", 100, 0, hourMinute(7, 0), {}}};
    plan2.visits = {Visit{"2", "John", 1, 80, hourMinute(8, 0), hourMinute(10, 0),
                          30 * 60, -1, NULL_TIME}};
    // Left out of every route == unassigned.
    Score s2 = ScoreCalculator::calculateScore(plan2);
    checkEqual("maximizeVisitsAssigned medium penalty", -s2.medium, 30);
    std::printf("\n");
}

// -------------------------------------------------------------------------
// The HardMediumSoft <-> engine HardSoft packing must round-trip exactly.
// -------------------------------------------------------------------------
void testScorePacking()
{
    std::printf("== HardMediumSoft <-> engine score packing ==\n");
    const std::vector<Score> cases = {
        {0, 0, 0},        {-5, 0, 0},        {0, -30, 0},
        {0, 0, -2423},    {-20, -30, -2423}, {-1, -899999, -999999999},
    };
    bool ok = true;
    for (const Score &s : cases)
    {
        const Score back = fromEngineScore(toEngineScore(s));
        if (!(back == s))
        {
            ok = false;
            std::printf("    round-trip failed: %s -> %s\n", s.toString().c_str(),
                        back.toString().c_str());
        }
    }
    check(ok, "packing round-trips on every level");

    // Lexicographic order must survive the packing: medium dominates soft.
    check(toEngineScore({0, -1, 0}).soft < toEngineScore({0, 0, -999999999}).soft,
          "one unassigned visit outranks any travel-time saving");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// The incremental calculator must agree with the full recompute after an
// arbitrary sequence of list mutations.
// -------------------------------------------------------------------------
void testIncrementalMatchesFullRecompute()
{
    std::printf("== Incremental calculator vs full recompute ==\n");
    VehicleRoutePlan plan = buildRandomInstance(40, 5, /*seed=*/11);
    plan.rebuildUnassignedPool();

    VehicleRoutingIncrementalScoreCalculator calculator;
    calculator.resetWorkingSolution(plan);

    using Descriptor = optimaru::SolutionDescriptor<VehicleRoutePlan>;
    std::mt19937_64 rng(3);
    bool ok = true;
    for (int step = 0; step < 400 && ok; ++step)
    {
        // Move a random visit from one list to another (routes and the
        // unassigned pool alike), through the calculator's hook protocol.
        const int listCount = Descriptor::listCount(plan);
        int from = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));
        if (Descriptor::listSize(plan, from) == 0)
            continue;
        const int position = static_cast<int>(
            rng() % static_cast<std::uint64_t>(Descriptor::listSize(plan, from)));
        const int to = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));

        calculator.beforeListChanged(from);
        const int element = Descriptor::removeElement(plan, from, position);
        calculator.afterListChanged(from);

        const int insertAt = static_cast<int>(
            rng() % static_cast<std::uint64_t>(Descriptor::listSize(plan, to) + 1));
        calculator.beforeListChanged(to);
        Descriptor::insertElement(plan, to, insertAt, element);
        calculator.afterListChanged(to);

        const Score incremental = calculator.currentScore();
        const Score full = ScoreCalculator::calculateScore(plan);
        if (!(incremental == full))
        {
            ok = false;
            std::printf("    step %d: incremental %s != full %s\n", step,
                        incremental.toString().c_str(), full.toString().c_str());
        }
    }
    check(ok, "incremental == full recompute over 400 random list moves");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// End-to-end: the engine must conserve visits, keep the pool consistent, and
// report a score that survives a from-scratch recompute.
// -------------------------------------------------------------------------
void testSolveIntegrity()
{
    std::printf("== Engine integration ==\n");
    VehicleRoutePlan plan = buildRandomInstance(60, 6, /*seed=*/5);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(3);
    config.randomSeed = 5;
    Solver(config).solve(plan);

    // Every visit appears exactly once across the routes and the pool.
    std::vector<int> seen(plan.visits.size(), 0);
    for (const Vehicle &vehicle : plan.vehicles)
    {
        for (int visitIndex : vehicle.visitIndices)
            ++seen[static_cast<std::size_t>(visitIndex)];
    }
    for (int visitIndex : plan.unassignedVisitIndices)
        ++seen[static_cast<std::size_t>(visitIndex)];
    bool conserved = true;
    for (int count : seen)
        conserved = conserved && count == 1;
    check(conserved, "every visit appears exactly once (routes + pool)");

    // The pool must agree with "in no route".
    VehicleRoutePlan copy = plan;
    copy.rebuildUnassignedPool();
    check(copy.unassignedVisitIndices == plan.unassignedVisitIndices,
          "unassigned pool matches the routes");

    const Score reported = plan.score;
    const Score recomputed = ScoreCalculator::calculateScore(plan);
    check(reported == recomputed, "reported score matches a full recompute");
    std::printf("    solved 60 visits / 6 vehicles in 3s: %s\n",
                plan.score.toString().c_str());

    // One vehicle plus the pool: route elimination must decline (there is no
    // other route to take the visits) and the repair must never drop an
    // element because the pool is the only list left.
    VehicleRoutePlan single = buildRandomInstance(8, 1, /*seed=*/3);
    Solver::Config singleConfig;
    singleConfig.spentLimit = std::chrono::seconds(1);
    singleConfig.randomSeed = 3;
    singleConfig.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    Solver(singleConfig).solve(single);
    std::vector<int> seenSingle(single.visits.size(), 0);
    for (int visitIndex : single.vehicles[0].visitIndices)
        ++seenSingle[static_cast<std::size_t>(visitIndex)];
    for (int visitIndex : single.unassignedVisitIndices)
        ++seenSingle[static_cast<std::size_t>(visitIndex)];
    bool singleConserved = true;
    for (int count : seenSingle)
        singleConserved = singleConserved && count == 1;
    check(singleConserved, "one vehicle: every visit appears exactly once");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// FULL_ASSERT drives the engine's own incremental-vs-full check on every
// move, including the ruin & recreate path with all five ruin operators.
// -------------------------------------------------------------------------
void testFullAssert()
{
    std::printf("== FULL_ASSERT (incremental score consistency) ==\n");
    VehicleRoutePlan plan = buildRandomInstance(25, 4, /*seed=*/9);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 9;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    // Kick often so the LNS path is genuinely exercised inside the budget.
    config.ruinRecreateUnimprovedStepTrigger = 50;
    config.ruinRecreatePolishSteps = 20;

    bool threw = false;
    try
    {
        Solver(config).solve(plan);
    }
    catch (const optimaru::ScoreCorruptionError &)
    {
        threw = true;
    }
    check(!threw, "moves + ruin & recreate pass FULL_ASSERT");

    // The iterated local search as the only strategy: every descent move is
    // previewed, applied and (on a rejected iteration) restored from a
    // snapshot, and FULL_ASSERT compares each preview with the applied score.
    VehicleRoutePlan ilsPlan = buildRandomInstance(40, 5, /*seed=*/11);
    Solver::Config ilsConfig;
    ilsConfig.spentLimit = std::chrono::seconds(2);
    ilsConfig.randomSeed = 11;
    ilsConfig.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    ilsConfig.iteratedLocalSearchWeight = 1.0;
    ilsConfig.iteratedLocalSearchMaxSegment = 3;
    threw = false;
    try
    {
        Solver(ilsConfig).solve(ilsPlan);
    }
    catch (const optimaru::ScoreCorruptionError &)
    {
        threw = true;
    }
    check(!threw, "iterated local search passes FULL_ASSERT");
    std::vector<int> seen(ilsPlan.visits.size(), 0);
    for (const Vehicle &vehicle : ilsPlan.vehicles)
        for (int visitIndex : vehicle.visitIndices)
            ++seen[static_cast<std::size_t>(visitIndex)];
    for (int visitIndex : ilsPlan.unassignedVisitIndices)
        ++seen[static_cast<std::size_t>(visitIndex)];
    bool conserved = true;
    for (int count : seen)
        conserved = conserved && count == 1;
    check(conserved, "iterated local search: every visit appears exactly once");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// The ALNS additions have to earn their place: on an instance with tight
// windows, the adaptive ruin & recreate must beat plain local search.
// -------------------------------------------------------------------------
void testAlnsBeatsPlainLocalSearch()
{
    std::printf("== ALNS vs plain local search ==\n");
    int alnsWins = 0;
    const int seeds = 3;
    for (int seed = 0; seed < seeds; ++seed)
    {
        VehicleRoutePlan plain = buildRandomInstance(80, 8, /*seed=*/100 + seed);
        VehicleRoutePlan alns = plain;

        Solver::Config base;
        base.spentLimit = std::chrono::seconds(3);
        base.randomSeed = static_cast<std::uint64_t>(seed);

        Solver::Config plainConfig = base;
        plainConfig.ruinRecreate = false;
        Solver(plainConfig).solve(plain);

        Solver::Config alnsConfig = base;
        alnsConfig.ruinRecreate = true;
        Solver(alnsConfig).solve(alns);

        std::printf("    seed %d: plain %s | ALNS %s\n", seed,
                    plain.score.toString().c_str(), alns.score.toString().c_str());
        if (alns.score >= plain.score)
            ++alnsWins;
    }
    check(alnsWins >= 2, "ALNS is at least as good on a majority of seeds");
    std::printf("\n");
}


// -------------------------------------------------------------------------
// The four local search strategies,
// each a MoveOperator plugin in core (impl/localsearch/strategy/):
// best-improvement granular descent over a nearby route pair,
// destroy-and-repair as an ordinary acceptor-judged move (rather than only a
// stagnation kick), emptying a whole route in one move, and the ejection
// chain (break capacity on purpose, then repair it). All four mutate through multi-step
// sequences, so their rollback is where a bug would hide — every combination
// runs under FULL_ASSERT, which recomputes the score from scratch after every
// single evaluation and throws the moment an undo leaves the incremental
// calculator disagreeing with reality.
// -------------------------------------------------------------------------
void testSearchStrategiesAreConsistent()
{
    std::printf("== Search strategy plugins (FULL_ASSERT) ==\n");
    struct Case
    {
        const char *label;
        double descent;
        double drMove;
        double routeElimination;
        double ejectionChain;
    };
    const Case cases[] = {
        {"none (plain move mix)", 0.0, 0.0, 0.0, 0.0},
        {"descent only", 0.05, 0.0, 0.0, 0.0},
        {"destroy-repair as move only", 0.0, 0.15, 0.0, 0.0},
        {"route elimination only", 0.0, 0.0, 0.15, 0.0},
        {"ejection chain only", 0.0, 0.0, 0.0, 0.15},
        {"descent + destroy-repair", 0.05, 0.15, 0.0, 0.0},
        {"all four, heavily weighted", 0.25, 0.35, 0.35, 0.35},
    };
    for (const Case &testCase : cases)
    {
        VehicleRoutePlan plan = buildRandomInstance(40, 5, /*seed=*/3);
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(2);
        config.randomSeed = 3;
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        config.descentWeight = testCase.descent;
        config.ruinRecreateAsMoveWeight = testCase.drMove;
        config.routeEliminationWeight = testCase.routeElimination;
        config.ejectionChainWeight = testCase.ejectionChain;
        config.ruinRecreateUnimprovedStepTrigger = 50;
        config.ruinRecreatePolishSteps = 10;

        bool threw = false;
        try
        {
            Solver(config).solve(plan);
        }
        catch (const optimaru::ScoreCorruptionError &)
        {
            threw = true;
        }

        std::vector<int> seen(plan.visits.size(), 0);
        for (const Vehicle &vehicle : plan.vehicles)
        {
            for (int visitIndex : vehicle.visitIndices)
                ++seen[static_cast<std::size_t>(visitIndex)];
        }
        for (int visitIndex : plan.unassignedVisitIndices)
            ++seen[static_cast<std::size_t>(visitIndex)];
        bool conserved = true;
        for (int count : seen)
            conserved = conserved && count == 1;

        const Score reported = plan.score;
        const bool matches = reported == ScoreCalculator::calculateScore(plan);
        check(!threw && conserved && matches, testCase.label);
    }
    std::printf("\n");
}


// -------------------------------------------------------------------------
// LocalSearchConfig::unassignedList: a repair may not park a visit it took
// off a route in the unassigned pool, however much cheaper the pool is than a
// capacity breach, while a visit the ruin took out of the pool may go back.
// Without the exclusion the same repair does use the pool, which is what made
// the kick stall on tightly loaded fleets.
// -------------------------------------------------------------------------
void testRepairNeverParksRoutedVisitsInThePool()
{
    std::printf("== Ruin repair keeps routed visits out of the pool ==\n");
    using Descriptor = optimaru::SolutionDescriptor<VehicleRoutePlan>;
    // Four vehicles whose capacity is far below the demand they start with:
    // every insertion into a route breaches capacity (hard), the pool costs
    // medium, so an unconstrained greedy repair prefers the pool.
    auto build = []
    {
        VehicleRoutePlan plan = buildRandomInstance(24, 4, /*seed=*/17);
        for (Visit &visit : plan.visits)
        {
            visit.demand = 10;
            visit.minStartTime = 0;
            visit.maxEndTime = 48 * 3600;
        }
        for (Vehicle &vehicle : plan.vehicles)
        {
            vehicle.capacity = 10;
            vehicle.departureTime = 0;
            vehicle.visitIndices.clear();
        }
        plan.unassignedVisitIndices.clear();
        for (std::size_t i = 0; i < plan.visits.size(); ++i)
        {
            if (i + 2 >= plan.visits.size())
                plan.unassignedVisitIndices.push_back(static_cast<int>(i));
            else
                plan.vehicles[i % plan.vehicles.size()].visitIndices.push_back(static_cast<int>(i));
        }
        return plan;
    };
    auto run = [&](int unassignedList, int &routedToPool, int &poolOriginRouted)
    {
        VehicleRoutePlan plan = build();
        const int pool = Descriptor::unassignedList(plan);

        VehicleRoutingIncrementalScoreCalculator calculator;
        optimaru::ScoreDirector<VehicleRoutePlan> director(plan, calculator);
        optimaru::ElementIndex<VehicleRoutePlan> index;
        index.build(plan);
        optimaru::NearbyNeighborhoods nearby;  // disabled: every position is a candidate
        optimaru::RuinRecreateConfig config;
        optimaru::GreedyRepair<VehicleRoutePlan> repair(index, nearby, config, unassignedList);

        // Six routed visits and one pool visit.
        const int poolVisit = plan.unassignedVisitIndices.front();
        std::vector<int> ruined = {0, 1, 2, 3, 4, 5, poolVisit};
        std::mt19937_64 rng(17);
        repair.ruin(director, ruined);
        repair.recreate(director, ruined, rng);

        routedToPool = 0;
        for (int e = 0; e < 6; ++e)
            if (index.listOf(e) == pool) ++routedToPool;
        poolOriginRouted = index.listOf(poolVisit) != pool ? 1 : 0;
        // The repair must still place every ruined visit exactly once.
        std::vector<int> seen(plan.visits.size(), 0);
        for (const Vehicle &vehicle : plan.vehicles)
            for (int visitIndex : vehicle.visitIndices)
                ++seen[static_cast<std::size_t>(visitIndex)];
        for (int visitIndex : plan.unassignedVisitIndices)
            ++seen[static_cast<std::size_t>(visitIndex)];
        return std::all_of(seen.begin(), seen.end(), [](int c) { return c == 1; });
    };

    int routedToPool = 0, poolOriginRouted = 0;
    const int pool = Descriptor::unassignedList(build());
    check(run(pool, routedToPool, poolOriginRouted), "excluded repair conserves every visit");
    check(routedToPool == 0, "excluded repair keeps all six routed visits on routes");
    std::printf("    pool-origin visit went back to a route: %d\n", poolOriginRouted);
    check(run(-1, routedToPool, poolOriginRouted), "unconstrained repair conserves every visit");
    check(routedToPool > 0, "without the exclusion the same repair parks routed visits in the pool");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// The ejection chain has to actually fire, not just survive FULL_ASSERT.
// Three vehicles of capacity 10 start as {6,4} {5,3} {4,3} with a demand-5
// visit left over: the total demand is exactly 30, and the only full
// assignment moves a 3 from the second route to the third so the 5 fits
// beside the other 5. Every route is too full for the 5 on its own, so with
// nothing but ejection chains on (weight 1.0, no ruin & recreate) the pool is
// only emptied if a chain inserts it overloaded and swaps the slack back.
// -------------------------------------------------------------------------
void testEjectionChainFillsTightPool()
{
    std::printf("== Ejection chain on a tightly packed fleet ==\n");
    VehicleRoutePlan plan;
    const int home = addLocation(plan, 49.20, 16.60);
    const double offsets[][2] = {{0.01, 0.00}, {0.00, 0.01}, {-0.01, 0.00},
                                 {0.00, -0.01}, {0.01, 0.01}, {-0.01, -0.01},
                                 {0.01, -0.01}};
    const int demands[] = {6, 4, 5, 3, 4, 3, 5};
    for (int i = 0; i < 7; ++i)
    {
        Visit visit;
        visit.id = std::to_string(i);
        visit.name = visit.id;
        visit.locationIndex
            = addLocation(plan, 49.20 + offsets[i][0], 16.60 + offsets[i][1]);
        visit.demand = demands[i];
        visit.minStartTime = 0;
        visit.maxEndTime = 24 * 3600;
        visit.unassignedPenaltyMinutes = 1000;
        plan.visits.push_back(visit);
    }
    HaversineDrivingTimeCalculator::initDrivingTimeMatrix(plan);
    for (int v = 0; v < 3; ++v)
        plan.vehicles.push_back(Vehicle{std::to_string(v), 10, home, 0, {}});
    plan.vehicles[0].visitIndices = {0, 1};
    plan.vehicles[1].visitIndices = {2, 3};
    plan.vehicles[2].visitIndices = {4, 5};
    // Visit 6 (demand 5) is in no route: the construction heuristic finds
    // every route over capacity and leaves it in the pool.

    // Several seeds, so the check is about the operator rather than one
    // lucky trajectory of the RNG.
    bool allFilled = true;
    for (std::uint64_t seed = 1; seed <= 3; ++seed)
    {
        VehicleRoutePlan run = plan;
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(5);
        config.stepCountLimit = 50;
        config.randomSeed = seed;
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        config.ruinRecreate = false;
        config.descentWeight = 0.0;
        config.ruinRecreateAsMoveWeight = 0.0;
        config.routeEliminationWeight = 0.0;
        config.iteratedLocalSearchWeight = 0.0;  // its ruin/repair could fill the pool too
        config.ejectionChainWeight = 1.0;
        Solver(config).solve(run);
        std::printf("    seed %llu: %s\n", static_cast<unsigned long long>(seed),
                    run.score.toString().c_str());
        allFilled = allFilled && run.score.hard == 0
                    && run.unassignedVisitIndices.empty();
    }
    check(allFilled, "every visit assigned within capacity on every seed");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Move previews must equal the applied score, bit for bit, for every move
// kind the engine samples: previews are what the search runs on, apply only
// following acceptance. A CVRP-shaped instance (windows never bind, no
// waiting) keeps previews enabled; the ScoreDirector applies each move for
// real, scores it, and undoes it to produce the reference.
// -------------------------------------------------------------------------
// `nonMetric` replaces the haversine matrix with random driving times that
// violate the triangle inequality and tightens the deadlines so some routes
// are late: a preview may then turn a late route into an on-time one and
// must drop the old lateness along with the old driving.
// `waiting` opens one visit's window after every departure: the prefix-sum
// cache is off and every preview is served by walking the candidate route
// (the same walk the cached pass falls back to when a route may be late).
// `staggered` gives every vehicle its own departure time and sends every
// other vehicle back home, so the per-list departure offset and the
// return-to-depot leg of a candidate route are exercised too.
void testPreviewsMatchAppliedScore(bool nonMetric, bool waiting = false,
                                   bool staggered = false)
{
    std::printf("== Move previews vs applied score (%s) ==\n",
                waiting     ? "waiting: previews by walking the candidate route"
                : staggered ? "staggered departures, round trips, late routes"
                : nonMetric ? "non-metric matrix, late routes"
                            : "metric matrix");
    VehicleRoutePlan plan = buildRandomInstance(30, 4, /*seed=*/21);
    std::mt19937_64 matrixRng(7);
    for (Visit &visit : plan.visits)
    {
        visit.minStartTime = 0;
        visit.maxEndTime = nonMetric ? 5 * 3600 : 48 * 3600;
    }
    if (waiting)
        plan.visits.front().minStartTime = 3600;  // opens after the 0s departure
    if (nonMetric)
    {
        for (auto &row : plan.drivingSeconds)
            for (Seconds &cell : row)
                cell = static_cast<Seconds>(matrixRng() % 3600);
    }
    for (std::size_t v = 0; v < plan.vehicles.size(); ++v)
    {
        Vehicle &vehicle = plan.vehicles[v];
        // Every window opens at 0, so a later departure never makes a visit
        // wait and previews stay on.
        vehicle.departureTime = staggered ? static_cast<Seconds>(600 * v) : 0;
        vehicle.returnToDepot = staggered && v % 2 == 1;
        vehicle.visitIndices.clear();
    }
    // Round-robin routes, the last four visits parked in the pool so that
    // pool-touching moves are covered too.
    plan.unassignedVisitIndices.clear();
    for (std::size_t i = 0; i < plan.visits.size(); ++i)
    {
        if (i + 4 >= plan.visits.size())
            plan.unassignedVisitIndices.push_back(static_cast<int>(i));
        else
            plan.vehicles[i % plan.vehicles.size()].visitIndices.push_back(static_cast<int>(i));
    }

    VehicleRoutingIncrementalScoreCalculator calculator;
    optimaru::ScoreDirector<VehicleRoutePlan> director(plan, calculator);
    const optimaru::testing::PreviewParity parity
        = optimaru::testing::runPreviewParity(director, calculator, /*seed=*/21, /*trials=*/3000);
    std::printf("    %d moves checked, %d previewed, %d declined\n", parity.checked,
                parity.previewed, parity.declined);
    // Tight deadlines (non-metric) send the cached path to the walk; a plan
    // with waiting walks every preview. Either way nothing is declined.
    check(parity.allEqual() && parity.previewed > 1000, "every preview equals the applied score");
    check(parity.declined == 0, "every preview is served");
    std::printf("\n");
}
}  // namespace

int main()
{
    testJavaFixtures();
    testScorePacking();
    testIncrementalMatchesFullRecompute();
    testPreviewsMatchAppliedScore(/*nonMetric=*/false);
    testPreviewsMatchAppliedScore(/*nonMetric=*/true);
    testPreviewsMatchAppliedScore(/*nonMetric=*/false, /*waiting=*/true);
    testPreviewsMatchAppliedScore(/*nonMetric=*/true, /*waiting=*/true);
    testPreviewsMatchAppliedScore(/*nonMetric=*/true, /*waiting=*/false, /*staggered=*/true);
    testSolveIntegrity();
    testFullAssert();
    testSearchStrategiesAreConsistent();
    testRepairNeverParksRoutedVisitsInThePool();
    testEjectionChainFillsTightPool();
    testAlnsBeatsPlainLocalSearch();

    if (failures == 0)
    {
        std::printf("All vehicle-routing tests PASSED.\n");
        return 0;
    }
    std::printf("%d vehicle-routing test(s) FAILED.\n", failures);
    return 1;
}

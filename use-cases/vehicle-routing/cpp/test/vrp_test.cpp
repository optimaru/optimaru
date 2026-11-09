// Vehicle-routing tests: the Java ConstraintProviderTest fixtures, the
// incremental calculator against the full recompute, and the engine
// integration (element conservation, unassigned pool, ALNS).
#include <chrono>
#include <algorithm>
#include <random>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "optimaru/editor.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/search/neighborhoods.hpp"
#include "optimaru/search/element_index.hpp"
#include "optimaru/search/greedy_repair.hpp"
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

    // vehicleCapacityCost: capacity 100, demands 80 + 40 -> 20 over.
    plan.vehicles = {Vehicle{"1", 100, home, hourMinute(7, 0), {}}};
    plan.visits = {
        Visit{"2", "John", loc2, 80, hourMinute(8, 0), hourMinute(10, 0), 30 * 60,
              -1, NULL_TIME},
        Visit{"3", "Paul", loc3, 40, hourMinute(8, 0), hourMinute(10, 0), 30 * 60,
              -1, NULL_TIME},
    };
    plan.vehicles[0].visitIndices = {0, 1};
    Score s = ScoreCalculator::score(plan);
    checkEqual("vehicleCapacityCost hard penalty", -s.hard, 20);
    // travelTimeCost: the route is open by default, so only
    // home->L2->L3 = 843 + 639 = 1482 is driven.
    checkEqual("travelTimeCost soft penalty (open route)", -s.soft, 1482);
    // With returnToDepot the Java round trip home->L2->L3->home is charged:
    // 843 + 639 + 941 = 2423.
    plan.vehicles[0].returnToDepot = true;
    s = ScoreCalculator::score(plan);
    checkEqual("travelTimeCost soft penalty (return to depot)", -s.soft, 2423);
    plan.vehicles[0].returnToDepot = false;

    // unassignedVisitsCost: one unassigned visit with a 30-minute service
    // duration is penalized by 30 on the medium level.
    VehicleRoutePlan plan2;
    addLocation(plan2, 49.288087, 16.562172);
    addLocation(plan2, 49.190922, 16.624466);
    HaversineDrivingTimeCalculator::initDrivingTimeMatrix(plan2);
    plan2.vehicles = {Vehicle{"1", 100, 0, hourMinute(7, 0), {}}};
    plan2.visits = {Visit{"2", "John", 1, 80, hourMinute(8, 0), hourMinute(10, 0),
                          30 * 60, -1, NULL_TIME}};
    // Left out of every route == unassigned.
    Score s2 = ScoreCalculator::score(plan2);
    checkEqual("unassignedVisitsCost medium penalty", -s2.medium, 30);
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
            std::printf("    round-trip failed: %s -> %s\n", s.str().c_str(),
                        back.str().c_str());
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
    calculator.rebuild(plan);

    using Descriptor = optimaru::SolutionTraits<VehicleRoutePlan>;
    std::mt19937_64 rng(3);
    bool ok = true;
    for (int step = 0; step < 400 && ok; ++step)
    {
        // Move a random visit from one list to another (routes and the
        // unassigned pool alike), through the calculator's hook protocol.
        const int list_count = Descriptor::list_count(plan);
        int from = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
        if (Descriptor::list_size(plan, from) == 0)
            continue;
        const int position = static_cast<int>(
            rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, from)));
        const int to = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));

        calculator.before_list_edit(from);
        const int element = Descriptor::remove_element(plan, from, position);
        calculator.after_list_edit(from);

        const int insert_at = static_cast<int>(
            rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, to) + 1));
        calculator.before_list_edit(to);
        Descriptor::insert_element(plan, to, insert_at, element);
        calculator.after_list_edit(to);

        const Score incremental = calculator.current_score();
        const Score full = ScoreCalculator::score(plan);
        if (!(incremental == full))
        {
            ok = false;
            std::printf("    step %d: incremental %s != full %s\n", step,
                        incremental.str().c_str(), full.str().c_str());
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
    config.time_limit = std::chrono::seconds(3);
    config.seed = 5;
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
    const Score recomputed = ScoreCalculator::score(plan);
    check(reported == recomputed, "reported score matches a full recompute");
    std::printf("    solved 60 visits / 6 vehicles in 3s: %s\n",
                plan.score.str().c_str());

    // One vehicle plus the pool: route elimination must decline (there is no
    // other route to take the visits) and the repair must never drop an
    // element because the pool is the only list left.
    VehicleRoutePlan single = buildRandomInstance(8, 1, /*seed=*/3);
    Solver::Config singleConfig;
    singleConfig.time_limit = std::chrono::seconds(1);
    singleConfig.seed = 3;
    singleConfig.score_checks = optimaru::ScoreChecks::Full;
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
// ScoreChecks::Full drives the engine's own incremental-vs-full check on every
// move, including the ruin & recreate path with all five ruin operators.
// -------------------------------------------------------------------------
void testFullAssert()
{
    std::printf("== FULL_ASSERT (incremental score consistency) ==\n");
    VehicleRoutePlan plan = buildRandomInstance(25, 4, /*seed=*/9);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(2);
    config.seed = 9;
    config.score_checks = optimaru::ScoreChecks::Full;
    // Kick often so the LNS path is genuinely exercised inside the budget.
    config.ruinRecreateUnimprovedStepTrigger = 50;
    config.ruinRecreatePolishSteps = 20;

    bool threw = false;
    try
    {
        Solver(config).solve(plan);
    }
    catch (const optimaru::ScoreMismatchError &)
    {
        threw = true;
    }
    check(!threw, "moves + ruin & recreate pass FULL_ASSERT");

    // The iterated local search as the only strategy: every descent move is
    // previewed, applied and (on a rejected iteration) restored from a
    // snapshot, and ScoreChecks::Full compares each preview with the applied score.
    VehicleRoutePlan ilsPlan = buildRandomInstance(40, 5, /*seed=*/11);
    Solver::Config ilsConfig;
    ilsConfig.time_limit = std::chrono::seconds(2);
    ilsConfig.seed = 11;
    ilsConfig.score_checks = optimaru::ScoreChecks::Full;
    ilsConfig.iterated_local_search_weight = 1.0;
    ilsConfig.iterated_local_search_max_segment = 3;
    threw = false;
    try
    {
        Solver(ilsConfig).solve(ilsPlan);
    }
    catch (const optimaru::ScoreMismatchError &)
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
        base.time_limit = std::chrono::seconds(3);
        base.seed = static_cast<std::uint64_t>(seed);

        Solver::Config plainConfig = base;
        plainConfig.ruin_recreate = false;
        Solver(plainConfig).solve(plain);

        Solver::Config alnsConfig = base;
        alnsConfig.ruin_recreate = true;
        Solver(alnsConfig).solve(alns);

        std::printf("    seed %d: plain %s | ALNS %s\n", seed,
                    plain.score.str().c_str(), alns.score.str().c_str());
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
// runs under ScoreChecks::Full, which recomputes the score from scratch after every
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
        double route_elimination;
        double ejection_chain;
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
        config.time_limit = std::chrono::seconds(2);
        config.seed = 3;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.descent_weight = testCase.descent;
        config.ruinRecreateAsMoveWeight = testCase.drMove;
        config.route_elimination_weight = testCase.route_elimination;
        config.ejection_chain_weight = testCase.ejection_chain;
        config.ruinRecreateUnimprovedStepTrigger = 50;
        config.ruinRecreatePolishSteps = 10;

        bool threw = false;
        try
        {
            Solver(config).solve(plan);
        }
        catch (const optimaru::ScoreMismatchError &)
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
        const bool matches = reported == ScoreCalculator::score(plan);
        check(!threw && conserved && matches, testCase.label);
    }
    std::printf("\n");
}


// -------------------------------------------------------------------------
// SearchSettings::unassigned_list: a repair may not park a visit it took
// off a route in the unassigned pool, however much cheaper the pool is than a
// capacity breach, while a visit the ruin took out of the pool may go back.
// Without the exclusion the same repair does use the pool, which is what made
// the kick stall on tightly loaded fleets.
// -------------------------------------------------------------------------
void testRepairNeverParksRoutedVisitsInThePool()
{
    std::printf("== Ruin repair keeps routed visits out of the pool ==\n");
    using Descriptor = optimaru::SolutionTraits<VehicleRoutePlan>;
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
    auto run = [&](int unassigned_list, int &routedToPool, int &poolOriginRouted)
    {
        VehicleRoutePlan plan = build();
        const int pool = Descriptor::unassigned_list(plan);

        VehicleRoutingIncrementalScoreCalculator calculator;
        optimaru::Editor<VehicleRoutePlan> editor(plan, calculator);
        optimaru::ElementIndex<VehicleRoutePlan> index;
        index.build(plan);
        optimaru::Neighborhoods nearby;  // disabled: every position is a candidate
        optimaru::RuinRecreateSettings config;
        optimaru::GreedyRepair<VehicleRoutePlan> repair(index, nearby, config, unassigned_list);

        // Six routed visits and one pool visit.
        const int poolVisit = plan.unassignedVisitIndices.front();
        std::vector<int> ruined = {0, 1, 2, 3, 4, 5, poolVisit};
        std::mt19937_64 rng(17);
        repair.ruin(editor, ruined);
        repair.recreate(editor, ruined, rng);

        routedToPool = 0;
        for (int e = 0; e < 6; ++e)
            if (index.list_of(e) == pool) ++routedToPool;
        poolOriginRouted = index.list_of(poolVisit) != pool ? 1 : 0;
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
    const int pool = Descriptor::unassigned_list(build());
    check(run(pool, routedToPool, poolOriginRouted), "excluded repair conserves every visit");
    check(routedToPool == 0, "excluded repair keeps all six routed visits on routes");
    std::printf("    pool-origin visit went back to a route: %d\n", poolOriginRouted);
    check(run(-1, routedToPool, poolOriginRouted), "unconstrained repair conserves every visit");
    check(routedToPool > 0, "without the exclusion the same repair parks routed visits in the pool");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// The ejection chain has to actually fire, not just survive ScoreChecks::Full.
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
        config.time_limit = std::chrono::seconds(5);
        config.step_limit = 50;
        config.seed = seed;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruin_recreate = false;
        config.descent_weight = 0.0;
        config.ruinRecreateAsMoveWeight = 0.0;
        config.route_elimination_weight = 0.0;
        config.iterated_local_search_weight = 0.0;  // its ruin/repair could fill the pool too
        config.ejection_chain_weight = 1.0;
        Solver(config).solve(run);
        std::printf("    seed %llu: %s\n", static_cast<unsigned long long>(seed),
                    run.score.str().c_str());
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
// waiting) keeps previews enabled; the Editor applies each move for
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
    optimaru::Editor<VehicleRoutePlan> editor(plan, calculator);
    const optimaru::testing::PreviewParity parity
        = optimaru::testing::run_preview_parity(editor, calculator, /*seed=*/21, /*trials=*/3000);
    std::printf("    %d moves checked, %d previewed, %d declined\n", parity.checked,
                parity.previewed, parity.declined);
    // Tight deadlines (non-metric) send the cached path to the walk; a plan
    // with waiting walks every preview. Either way nothing is declined.
    check(parity.all_equal() && parity.previewed > 1000, "every preview equals the applied score");
    check(parity.declined == 0, "every preview is served");
    std::printf("\n");
}
// -------------------------------------------------------------------------
// stop_at_first_feasible's best-score limit (0, INT64_MIN, INT64_MIN): the
// engine compares hard only (soft -inf), so no soft score, however low,
// misses the stop. Through toEngineScore the sentinel folded into a finite
// engine soft that a soft score below it never reached.
// -------------------------------------------------------------------------
void verifyStopAtFirstFeasibleLimit()
{
    std::printf("== stop_at_first_feasible's limit is hard-only ==\n");
    const optimaru::Score limit
        = toEngineScoreLimit(Score{0, kNoLevelFloor, kNoLevelFloor});
    check(limit.hard == 0 && std::isinf(limit.soft) && limit.soft < 0.0,
          "(0, INT64_MIN, INT64_MIN) -> engine limit (0, -inf)");
    check(optimaru::Score{0, -1e30} >= limit
              && toEngineScore(Score{0, -5, -(1LL << 62)}) >= limit,
          "a feasible score with any soft (-1e30; medium -5, soft -2^62) reaches it");
    check(!(optimaru::Score{-1, 0.0} >= limit),
          "an infeasible score does not");
    check(toEngineScoreLimit(Score{0, kNoLevelFloor, 0}).soft
                  == -std::numeric_limits<double>::infinity()
              && toEngineScoreLimit(Score{0, 0, kNoLevelFloor}).soft
                     == -std::numeric_limits<double>::infinity(),
          "INT64_MIN on medium or on soft drops the soft-side floor");
    const Score other{-2, -3, -5};
    check(toEngineScoreLimit(other) == toEngineScore(other),
          "any other limit folds like a score");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyStopAtFirstFeasibleLimit();
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

// Pickup-and-delivery tests: the coupling constraints, the incremental
// calculator against the full recompute, and the engine integration.
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "pdp/demo_data.hpp"
#include "pdp/domain.hpp"
#include "pdp/haversine.hpp"
#include "pdp/incremental_calculator.hpp"
#include "pdp/score_calculator.hpp"
#include "pdp/solver.hpp"

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

// A two-request instance with one vehicle, built by hand so the constraint
// expectations are exact. `demand` is per request; the vehicle's capacity is
// 10, so demand 4 leaves both requests carryable at once and demand 6 does not
// — which is how the capacity case is isolated from the coupling cases.
PdpRoutePlan buildTinyInstance(int demand = 4)
{
    PdpRoutePlan plan;
    plan.name = "tiny";
    const int home = addLocation(plan, 43.77, 11.25);
    const int p1 = addLocation(plan, 43.78, 11.26);
    const int d1 = addLocation(plan, 43.79, 11.27);
    const int p2 = addLocation(plan, 43.76, 11.24);
    const int d2 = addLocation(plan, 43.75, 11.23);

    plan.vehicles = {Vehicle{"1", 10, home, hourMinute(7, 0), {}}};
    plan.nodes = {
        Node{"p1",
             p1,
             hourMinute(8, 0),
             hourMinute(18, 0),
             300,
             0,
             true,
             -1,
             NULL_TIME},
        Node{"d1",
             d1,
             hourMinute(8, 0),
             hourMinute(18, 0),
             300,
             0,
             false,
             -1,
             NULL_TIME},
        Node{"p2",
             p2,
             hourMinute(8, 0),
             hourMinute(18, 0),
             300,
             1,
             true,
             -1,
             NULL_TIME},
        Node{"d2",
             d2,
             hourMinute(8, 0),
             hourMinute(18, 0),
             300,
             1,
             false,
             -1,
             NULL_TIME},
    };
    plan.deliveries
        = {Delivery{"r1", 0, 1, demand}, Delivery{"r2", 2, 3, demand}};
    HaversineDrivingTimeCalculator::initDrivingTimeMatrix(plan);
    return plan;
}

void testCouplingConstraints()
{
    std::printf("== PDP coupling constraints ==\n");
    const std::int64_t penalty = ScoreCalculator::kCouplingPenalty;

    // Legal: p1 d1 p2 d2 on one vehicle.
    PdpRoutePlan legal = buildTinyInstance();
    legal.vehicles[0].nodeIndices = {0, 1, 2, 3};
    legal.rebuildUnassignedPool();
    Score s = ScoreCalculator::calculateScore(legal);
    check(s.hard == 0, "a properly ordered pair is feasible");
    check(s.medium == 0, "nothing unassigned when every stop is routed");
    check(s.soft < 0, "travel time is penalized on the soft level");
    {
        // The route is open by default: home -> p1 -> d1 -> p2 -> d2 with no
        // leg back. Turning returnToDepot on adds exactly d2 -> home.
        const std::vector<int> &route = legal.vehicles[0].nodeIndices;
        Seconds open = 0;
        int previous = legal.vehicles[0].homeLocationIndex;
        for (int nodeIndex : route)
        {
            const int location
                = legal.nodes[static_cast<std::size_t>(nodeIndex)]
                      .locationIndex;
            open += legal.drivingTime(previous, location);
            previous = location;
        }
        const Seconds back
            = legal.drivingTime(previous, legal.vehicles[0].homeLocationIndex);
        check(back > 0, "the closing leg of the tiny instance is not free");
        check(-s.soft == open, "an open route charges no leg back home");
        legal.vehicles[0].returnToDepot = true;
        const Score closed = ScoreCalculator::calculateScore(legal);
        check(-closed.soft == open + back,
              "returnToDepot adds exactly the last-stop -> home leg");
        legal.vehicles[0].returnToDepot = false;
        ScoreCalculator::calculateScore(legal);
    }

    // Dropoff before pickup.
    PdpRoutePlan reversed = buildTinyInstance();
    reversed.vehicles[0].nodeIndices = {1, 0, 2, 3};
    reversed.rebuildUnassignedPool();
    check(ScoreCalculator::calculateScore(reversed).hard == -penalty,
          "dropoff before pickup is penalized once");

    // Half-assigned: p1 routed, d1 left out.
    PdpRoutePlan half = buildTinyInstance();
    half.vehicles[0].nodeIndices = {0, 2, 3};
    half.rebuildUnassignedPool();
    check(ScoreCalculator::calculateScore(half).hard == -penalty,
          "a half-routed request is penalized once");

    // Both stops unassigned: medium, not hard.
    PdpRoutePlan dropped = buildTinyInstance();
    dropped.vehicles[0].nodeIndices = {2, 3};
    dropped.rebuildUnassignedPool();
    Score droppedScore = ScoreCalculator::calculateScore(dropped);
    check(droppedScore.hard == 0,
          "a fully dropped request is not hard-infeasible");
    check(droppedScore.medium == -10,
          "a dropped request costs its two service durations (5+5 min)");

    // Capacity: with demand 6 each, carrying both at once is 12 > capacity 10.
    PdpRoutePlan overloaded = buildTinyInstance(/*demand=*/6);
    overloaded.vehicles[0].nodeIndices = {0, 2, 1, 3};  // p1 p2 d1 d2
    overloaded.rebuildUnassignedPool();
    check(ScoreCalculator::calculateScore(overloaded).hard == -2,
          "the running-load peak is what breaks capacity (12 > 10)");

    // The same demand carried one request after the other fits fine — the
    // distinction that makes this a running-load check and not a route total.
    PdpRoutePlan sequential = buildTinyInstance(/*demand=*/6);
    sequential.vehicles[0].nodeIndices = {0, 1, 2, 3};
    sequential.rebuildUnassignedPool();
    check(ScoreCalculator::calculateScore(sequential).hard == 0,
          "the same demand is feasible when the requests do not overlap");

    // Split across vehicles.
    PdpRoutePlan split = buildTinyInstance();
    split.vehicles.push_back(Vehicle{"2", 10, 0, hourMinute(7, 0), {}});
    split.vehicles[0].nodeIndices = {0, 2, 3};
    split.vehicles[1].nodeIndices = {1};
    split.rebuildUnassignedPool();
    check(ScoreCalculator::calculateScore(split).hard == -penalty,
          "a pair split across two vehicles is penalized once");
    std::printf("\n");
}

void testIncrementalMatchesFullRecompute()
{
    std::printf("== Incremental calculator vs full recompute ==\n");
    PdpRoutePlan plan = buildRandomInstance(25, 4, /*seed=*/11);
    plan.rebuildUnassignedPool();

    PdpIncrementalScoreCalculator calculator;
    calculator.resetWorkingSolution(plan);

    using Descriptor = optimaru::SolutionDescriptor<PdpRoutePlan>;
    std::mt19937_64 rng(3);
    bool ok = true;
    for (int step = 0; step < 500 && ok; ++step)
    {
        const int listCount = Descriptor::listCount(plan);
        const int from
            = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));
        if (Descriptor::listSize(plan, from) == 0)
            continue;
        const int position = static_cast<int>(
            rng()
            % static_cast<std::uint64_t>(Descriptor::listSize(plan, from)));
        const int to
            = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));

        calculator.beforeListChanged(from);
        const int element = Descriptor::removeElement(plan, from, position);
        calculator.afterListChanged(from);

        // Score in the transient half-assigned state too: the engine does
        // exactly this between a remove and the matching insert.
        if (!(calculator.currentScore()
              == ScoreCalculator::calculateScore(plan)))
        {
            ok = false;
            std::printf("    step %d (mid-move): incremental %s != full %s\n",
                        step,
                        calculator.currentScore().toString().c_str(),
                        plan.score.toString().c_str());
            break;
        }

        const int insertAt = static_cast<int>(
            rng()
            % static_cast<std::uint64_t>(Descriptor::listSize(plan, to) + 1));
        calculator.beforeListChanged(to);
        Descriptor::insertElement(plan, to, insertAt, element);
        calculator.afterListChanged(to);

        const Score incremental = calculator.currentScore();
        const Score full = ScoreCalculator::calculateScore(plan);
        if (!(incremental == full))
        {
            ok = false;
            std::printf("    step %d: incremental %s != full %s\n",
                        step,
                        incremental.toString().c_str(),
                        full.toString().c_str());
        }
    }
    check(ok, "incremental == full recompute over 500 random list moves");
    std::printf("\n");
}

void testSolveIntegrity()
{
    std::printf("== Engine integration ==\n");
    PdpRoutePlan plan = buildRandomInstance(40, 5, /*seed=*/5);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(5);
    config.randomSeed = 5;
    Solver(config).solve(plan);

    std::vector<int> seen(plan.nodes.size(), 0);
    for (const Vehicle &vehicle : plan.vehicles)
    {
        for (int nodeIndex : vehicle.nodeIndices)
            ++seen[static_cast<std::size_t>(nodeIndex)];
    }
    for (int nodeIndex : plan.unassignedNodeIndices)
        ++seen[static_cast<std::size_t>(nodeIndex)];
    bool conserved = true;
    for (int count : seen)
        conserved = conserved && count == 1;
    check(conserved, "every stop appears exactly once (routes + pool)");

    const Score reported = plan.score;
    check(reported == ScoreCalculator::calculateScore(plan),
          "reported score matches a full recompute");
    check(plan.score.isFeasible(),
          "the solution is feasible: every pair coupled, ordered and in "
          "capacity");

    int routed = 0;
    for (const Delivery &delivery : plan.deliveries)
    {
        if (plan.nodes[static_cast<std::size_t>(delivery.pickupNodeIndex)]
                .vehicleIndex
            >= 0)
            ++routed;
    }
    std::printf("    40 requests / 5 vehicles in 5s: %s (%d/%d routed)\n",
                plan.score.toString().c_str(),
                routed,
                static_cast<int>(plan.deliveries.size()));
    std::printf("\n");
}

void testFullAssert()
{
    std::printf("== FULL_ASSERT (incremental score consistency) ==\n");
    PdpRoutePlan plan = buildRandomInstance(15, 3, /*seed=*/9);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(3);
    config.randomSeed = 9;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
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
    check(!threw, "pair moves + pair ruin + recreate pass FULL_ASSERT");
    std::printf("\n");
}

// Tightens an instance until requests genuinely have to be dropped: fewer
// seats and narrower windows than the generator's defaults.
PdpRoutePlan tighten(PdpRoutePlan plan, int capacity, Seconds windowMinutes)
{
    for (Vehicle &vehicle : plan.vehicles)
    {
        vehicle.capacity = capacity;
        // The A/B below was measured on classic round trips. Open routes (the
        // default) loosen the soft landscape enough that a 4-second budget no
        // longer separates the two configurations reliably, so the fixture
        // keeps the closed routes the measurements were taken on.
        vehicle.returnToDepot = true;
    }
    for (Node &node : plan.nodes)
        node.maxEndTime = node.minStartTime + windowMinutes * 60;
    return plan;
}

// What the pair-aware operators are actually for.
//
// Measured across instance tightness (5 seeds each, 4s budget): on a LOOSE
// instance they are a wash (3 wins / 2 losses) — the built-in element moves
// already route every request, so the two configurations only differ by noise
// in travel time. Their value shows up once capacity and time windows force
// requests to be dropped: 4/1 on both the medium and tight settings, and the
// gap is on the MEDIUM level, not soft. On the tightest seeds the built-ins
// leave roughly twice as many requests unrouted (-320 vs -160 medium).
//
// That is the expected shape, and it is the reason the operators exist:
// moving a request into or out of the unassigned pool is only reachable as a
// pair move. A single stop moved into the pool leaves a half-assigned request,
// which is hard-infeasible and therefore always rejected — so without a pair
// move the set of routed requests is frozen at whatever the construction
// heuristic produced, and the search can only reorder within it.
//
// The check runs on a step budget, not wall clock, and compares the MEDIUM
// level only. With 4s per solve the outcome depended on the machine: a slow
// CI runner got 2/3 and failed, twice on soft-level ties of the medium level
// the claim is about. At 100k steps every seed routes more requests with the
// pair-aware operators (5/0), and the same seed gives the same result anywhere.
void testPairOperatorsMatter()
{
    std::printf(
        "== Pair-aware operators vs built-ins alone (tight instance) ==\n");
    int pairWins = 0;
    int builtinWins = 0;
    const int seeds = 5;
    for (int seed = 0; seed < seeds; ++seed)
    {
        PdpRoutePlan without
            = tighten(buildRandomInstance(60, 3, /*seed=*/300 + seed),
                      /*capacity=*/6,
                      /*windowMinutes=*/120);
        PdpRoutePlan with = without;

        Solver::Config base;
        base.stepCountLimit = 100'000;
        base.spentLimit = std::chrono::minutes(1);  // safety net only
        base.randomSeed = static_cast<std::uint64_t>(seed);

        Solver::Config withoutConfig = base;
        withoutConfig.pairMoves = false;
        withoutConfig.pairRuin = false;
        Solver(withoutConfig).solve(without);

        Solver(base).solve(with);

        std::printf("    seed %d: built-ins only %-28s | pair-aware %s\n",
                    seed,
                    without.score.toString().c_str(),
                    with.score.toString().c_str());
        if (with.score.medium > without.score.medium)
            ++pairWins;
        else if (without.score.medium > with.score.medium)
            ++builtinWins;
    }
    check(pairWins > builtinWins,
          "pair-aware operators route more requests on a tight instance");
    std::printf("\n");
}

}  // namespace

int main()
{
    testCouplingConstraints();
    testIncrementalMatchesFullRecompute();
    testSolveIntegrity();
    testFullAssert();
    testPairOperatorsMatter();

    if (failures == 0)
    {
        std::printf("All pickup-delivery tests PASSED.\n");
        return 0;
    }
    std::printf("%d pickup-delivery test(s) FAILED.\n", failures);
    return 1;
}

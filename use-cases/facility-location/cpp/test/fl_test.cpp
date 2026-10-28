// Facility-location unit tests (engine test/ style: check() + non-zero exit on
// any failure):
//   (a) every constraint verified in isolation against hand-computed scores
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's ScoreDirector protocol (before/set/after)
//   (c) a small instance solved under EnvironmentMode::FULL_ASSERT
//   (d) a demo-scale instance reaches 0hard with every consumer assigned
//   (e) the parallel multi-start path (its own workers, so CloseFacilityMove
//       is registered in each) returns a consistent best solution
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "fl/domain.hpp"
#include "fl/incremental_calculator.hpp"
#include "fl/score_calculator.hpp"
#include "fl/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace fl;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

void checkScore(const std::string &label,
                const Score &actual,
                long expectedHard,
                double expectedSoft)
{
    const bool ok = (actual.hard == expectedHard
                     && std::fabs(actual.soft - expectedSoft) < 1e-9);
    std::printf("  [%s] %-42s got=%s expected=%ldhard/%gsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedSoft);
    if (!ok)
        ++failures;
}

void addFacility(FacilityLocationProblem &problem,
                 double latitude,
                 double longitude,
                 std::int64_t setupCost,
                 std::int64_t capacity)
{
    Facility facility;
    facility.id = "F" + std::to_string(problem.facilities.size());
    facility.location = {latitude, longitude};
    facility.setupCost = setupCost;
    facility.capacity = capacity;
    problem.facilities.push_back(facility);
}

void addConsumer(FacilityLocationProblem &problem,
                 double latitude,
                 double longitude,
                 std::int64_t demand,
                 int facility = -1)
{
    Consumer consumer;
    consumer.id = "C" + std::to_string(problem.consumers.size());
    consumer.location = {latitude, longitude};
    consumer.demand = demand;
    consumer.facility = facility;
    problem.consumers.push_back(consumer);
}

// -------------------------------------------------------------------------
// (a) Each constraint in isolation, full recompute vs hand-computed scores.
// Degree offsets are chosen binary-exact (0.25, 0.75, 1.0) so the expected
// meter distances are unambiguous: 0.25 deg = 27750 m, and the (0.75, 1.0)
// right triangle gives 1.25 deg = 138750 m.
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Verifying each constraint in isolation ==\n");

    // hard 1: facilityCapacity — demand 30 on capacity 20 -> -10.
    {
        FacilityLocationProblem p;
        addFacility(p, 0, 0, /*setupCost*/ 0, /*capacity*/ 20);
        addConsumer(p, 0, 0, /*demand*/ 15, /*facility*/ 0);
        addConsumer(p, 0, 0, /*demand*/ 15, /*facility*/ 0);
        checkScore("facilityCapacity overflow",
                   ScoreCalculator::calculateScore(p),
                   -10,
                   0);
    }
    // hard 1: demand within capacity costs nothing.
    {
        FacilityLocationProblem p;
        addFacility(p, 0, 0, 0, 20);
        addConsumer(p, 0, 0, 15, 0);
        checkScore("facilityCapacity within",
                   ScoreCalculator::calculateScore(p),
                   0,
                   0);
    }
    // hard 1: Java groups consumers by facility, so a facility no consumer
    // uses is never penalized — even with a pathological negative capacity.
    {
        FacilityLocationProblem p;
        addFacility(p, 0, 0, 0, -5);  // unused
        addFacility(p, 0, 0, 0, 20);
        addConsumer(p, 0, 0, 15, 1);
        checkScore("facilityCapacity ignores unused",
                   ScoreCalculator::calculateScore(p),
                   0,
                   0);
    }
    // soft 2: facilitySetupCost — one used facility, weight 2.
    {
        FacilityLocationProblem p;
        addFacility(p, 0, 0, /*setupCost*/ 1000, /*capacity*/ 100);
        addConsumer(p, 0, 0, 1, 0);
        checkScore("setupCost used once",
                   ScoreCalculator::calculateScore(p),
                   0,
                   -2000);
    }
    // soft 2: a second consumer on the same facility adds no setup cost.
    {
        FacilityLocationProblem p;
        addFacility(p, 0, 0, 1000, 100);
        addConsumer(p, 0, 0, 1, 0);
        addConsumer(p, 0, 0, 1, 0);
        checkScore("setupCost not repeated",
                   ScoreCalculator::calculateScore(p),
                   0,
                   -2000);
    }
    // soft 2: every used facility pays its own setup cost.
    {
        FacilityLocationProblem p;
        addFacility(p, 0, 0, 1000, 100);
        addFacility(p, 0, 0, 500, 100);
        addConsumer(p, 0, 0, 1, 0);
        addConsumer(p, 0, 0, 1, 1);
        checkScore("setupCost per used facility",
                   ScoreCalculator::calculateScore(p),
                   0,
                   -3000);
    }
    // soft 3: distanceFromFacility — 0.25 deg = 27750 m, weight 6.
    {
        FacilityLocationProblem p;
        addFacility(p, 0, 0, 0, 100);
        addConsumer(p, 0, 0.25, 1, 0);
        checkScore("distance 0.25 deg x weight 6",
                   ScoreCalculator::calculateScore(p),
                   0,
                   -166500);
    }
    // soft 3: Euclidean composition — (0.75, 1.0) -> 1.25 deg = 138750 m.
    {
        FacilityLocationProblem p;
        addFacility(p, 0, 0, 0, 100);
        addConsumer(p, 0.75, 1.0, 1, 0);
        checkScore("distance 3-4-5 triangle",
                   ScoreCalculator::calculateScore(p),
                   0,
                   -832500);
    }
    // The solution-level weight override: the Java provider declares the
    // distance constraint at ofSoft(5); the quickstart solution's
    // ConstraintWeightOverrides raises it to 6 (our default). Setting the
    // field back to 5 reproduces the un-overridden provider.
    {
        FacilityLocationProblem p;
        p.distanceFromFacilityWeight = 5;
        addFacility(p, 0, 0, 0, 100);
        addConsumer(p, 0, 0.25, 1, 0);
        checkScore("distance weight override -> 5",
                   ScoreCalculator::calculateScore(p),
                   0,
                   -138750);
    }
    // Unassigned consumers contribute nothing (no setup, distance, demand).
    {
        FacilityLocationProblem p;
        addFacility(p, 0, 0, 1000, 5);
        addConsumer(p, 0, 0.25, 10);  // unassigned
        checkScore("unassigned contributes nothing",
                   ScoreCalculator::calculateScore(p),
                   0,
                   0);
    }
    // All three constraints at once, hand-computed:
    //   A(setup 100, cap 10) serves demand 8 + 7 -> hard -5
    //   B(setup 200, cap 50) serves one consumer 0.25 deg away -> 27750 m
    //   soft = -(2 * (100 + 200) + 6 * 27750) = -167100
    {
        FacilityLocationProblem p;
        addFacility(p, 0, 0, 100, 10);
        addFacility(p, 0, 1.0, 200, 50);
        addConsumer(p, 0, 0, 8, 0);
        addConsumer(p, 0, 0, 7, 0);
        addConsumer(p, 0, 0.75, 3, 1);
        checkScore("combined", ScoreCalculator::calculateScore(p), -5, -167100);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random mid-size instance for the incremental cross-check and the solves.
// Total capacity `slack` times the total demand, spread evenly.
// -------------------------------------------------------------------------
FacilityLocationProblem buildRandomInstance(int facilityCount,
                                            int consumerCount,
                                            std::uint64_t seed,
                                            std::int64_t demandEach = 15,
                                            std::int64_t slack = 5)
{
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> latitudes(51.44, 51.56);
    std::uniform_real_distribution<double> longitudes(-0.16, -0.01);
    std::uniform_int_distribution<std::int64_t> setupCosts(20000, 80000);

    FacilityLocationProblem problem;
    problem.southWestCorner = {51.44, -0.16};
    problem.northEastCorner = {51.56, -0.01};
    const std::int64_t capacityEach
        = slack * demandEach * consumerCount / facilityCount;
    for (int f = 0; f < facilityCount; ++f)
    {
        addFacility(problem,
                    latitudes(rng),
                    longitudes(rng),
                    setupCosts(rng),
                    capacityEach);
    }
    for (int c = 0; c < consumerCount; ++c)
    {
        addConsumer(problem, latitudes(rng), longitudes(rng), demandEach);
    }
    return problem;
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves (including unassignments), driven through the ScoreDirector so
// the before/set/after hook contract is exercised exactly like in the
// engine.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Verifying incremental scoring == full recompute ==\n");
    FacilityLocationProblem p
        = buildRandomInstance(/*facilities*/ 8,
                              /*consumers*/ 40,
                              /*seed*/ 7,
                              /*demandEach*/ 15,
                              /*slack*/ 1);  // tight: over-capacity happens

    FacilityLocationIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<FacilityLocationProblem> director(p, calc);
    FacilityLocationProblem shadow = p;

    std::mt19937_64 rng(123);
    const int consumerCount = p.consumerCount();
    const int valueCount = p.facilityCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            // ChangeMove; 10% of the time to unassigned (-1), like the
            // construction heuristic's starting state.
            const int consumer = static_cast<int>(rng() % consumerCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(consumer, value);
        }
        else
        {
            // SwapMove.
            const int a = static_cast<int>(rng() % consumerCount);
            const int b = static_cast<int>(rng() % consumerCount);
            const int va = p.consumers[a].facility;
            const int vb = p.consumers[b].facility;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < consumerCount; ++i)
        {
            shadow.consumers[i].facility = p.consumers[i].facility;
        }
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = director.calculateScore();
        if (full.hard != incremental.hard
            || std::fabs(full.soft - incremental.soft) > 1e-9)
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
              + " random moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) FULL_ASSERT solve: the engine cross-checks the incremental score
// against a full reset + recompute after every single move and throws
// ScoreCorruptionError on any divergence.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (c) FULL_ASSERT solve ==\n");
    FacilityLocationProblem p = buildRandomInstance(/*facilities*/ 6,
                                                    /*consumers*/ 20,
                                                    /*seed*/ 11,
                                                    /*demandEach*/ 15,
                                                    /*slack*/ 2);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    // Kick early so the LNS ruin/recreate path also runs under FULL_ASSERT.
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    bool corrupted = false;
    Score final{};
    try
    {
        Solver solver(config);
        solver.solve(p);
        final = p.score;
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(p);
        check(final.hard == recomputed.hard
                  && std::fabs(final.soft - recomputed.soft) < 1e-9,
              "solver-reported score == full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Demo-scale solve: the Java demo instance shape (30 facilities x 150
// capacity, 60 consumers x 15 demand) must reach 0hard with every consumer
// assigned, well inside a few seconds.
// -------------------------------------------------------------------------
void verifyDemoScaleSolve()
{
    std::printf("== (d) Demo-scale solve reaches 0hard ==\n");
    FacilityLocationProblem p = buildRandomInstance(/*facilities*/ 30,
                                                    /*consumers*/ 60,
                                                    /*seed*/ 21,
                                                    /*demandEach*/ 15,
                                                    /*slack*/ 5);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(3);
    config.randomSeed = 5;
    Solver solver(config);
    solver.solve(p);

    bool allAssigned = true;
    for (const Consumer &consumer : p.consumers)
    {
        if (!consumer.isAssigned())
            allAssigned = false;
    }
    check(allAssigned, "every consumer assigned after the solve");
    check(p.score.hard == 0, "demo-scale instance reaches 0hard");
    const Score recomputed = ScoreCalculator::calculateScore(p);
    check(p.score.hard == recomputed.hard
              && std::fabs(p.score.soft - recomputed.soft) < 1e-9,
          "solver-reported score == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) Parallel multi-start: parallelSolverCount > 1 runs one worker Solver
// per start (rather than the engine's built-in multi-start, which would drop
// the registered CloseFacilityMove), so this guards that the winning copy is
// returned intact and its reported score is the real one.
// -------------------------------------------------------------------------
void verifyParallelSolve()
{
    std::printf("== (e) Parallel multi-start solve ==\n");
    FacilityLocationProblem p = buildRandomInstance(/*facilities*/ 20,
                                                    /*consumers*/ 40,
                                                    /*seed*/ 31,
                                                    /*demandEach*/ 15,
                                                    /*slack*/ 4);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 9;
    config.parallelSolverCount = 3;
    Solver solver(config);
    solver.solve(p);

    bool allAssigned = true;
    for (const Consumer &consumer : p.consumers)
    {
        if (!consumer.isAssigned())
            allAssigned = false;
    }
    check(allAssigned, "every consumer assigned after the parallel solve");
    check(p.score.hard == 0, "parallel solve reaches 0hard");
    const Score recomputed = ScoreCalculator::calculateScore(p);
    check(p.score.hard == recomputed.hard
              && std::fabs(p.score.soft - recomputed.soft) < 1e-9,
          "winning worker's score == full recompute");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifyDemoScaleSolve();
    verifyParallelSolve();

    if (failures == 0)
    {
        std::printf("All facility-location checks PASSED.\n");
        return 0;
    }
    std::printf("%d facility-location check(s) FAILED.\n", failures);
    return 1;
}

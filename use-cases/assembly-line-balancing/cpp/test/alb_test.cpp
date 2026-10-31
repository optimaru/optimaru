// Assembly-line-balancing unit tests, monolithic path (engine test/ style:
// check() + non-zero exit on any failure):
//   (a) every constraint against hand-computed raw totals: a positive case
//       (fixture H) and a zero case (fixture Z / an empty line) each, plus the
//       default score
//   (b) edge cases: empty problem, empty middle stations, stations at or
//       above the task count, absent limits, duplicate arcs, unassigned tasks
//   (c) the construction ported from the 0.1.5 package (plain, seeds, fixed
//       override, overflow past the task count, cycles)
//   (d) incremental score and every maintained raw total == full recompute
//       across a long random walk of change / swap / unassign / batch moves
//       through the ScoreDirector, each constraint non-zero at least once
//   (e) FULL_ASSERT solves (CloseStationMove, value recommender, ALNS)
//   (f) seeded determinism, the parallel multi-start, a SALBP-like instance
//       reaching 0hard, a complete seed surviving a zero budget
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "alb/construction.hpp"
#include "alb/domain.hpp"
#include "alb/incremental_calculator.hpp"
#include "alb/score_calculator.hpp"
#include "alb/solver.hpp"
#include "alb_fixtures.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace alb;
using namespace alb_test;

namespace
{

RawCosts rawOf(AssemblyLineBalancingProblem p)
{
    ScoreCalculator::prepare(p);
    return ScoreCalculator::rawCosts(p);
}

Score scoreOf(AssemblyLineBalancingProblem p)
{
    ScoreCalculator::prepare(p);
    return ScoreCalculator::calculateScore(p);
}

// -------------------------------------------------------------------------
// (a) Per-constraint hand checks.
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Per-constraint raw costs by hand ==\n");
    const RawCosts h = rawOf(fixtureH());
    check(h.stationCapacityCost == 1, "stationCapacityCost positive: station 3 carries 6 of 5 -> 1");
    check(h.taskPrecedenceCost == 1, "taskPrecedenceCost positive: A on 1 before B on 0 -> 1");
    check(h.maxStationsCost == 1, "maxStationsCost positive: D on 3 with max_stations 3 -> 1");
    check(h.taskStationBoundsCost == 2, "taskStationBoundsCost positive: B below 1, D above 2 -> 2");
    check(h.fixedTaskStationsCost == 1, "fixedTaskStationsCost positive: A off its fixed 0 -> 1");
    check(h.forbiddenTaskStationsCost == 1,
          "forbiddenTaskStationsCost positive: A on 1, listed twice -> 1");
    check(h.sameStationGroupsCost == 1, "sameStationGroupsCost positive: {B, A, C} on {0, 1} -> 1");
    check(h.differentStationPairsCost == 2,
          "differentStationPairsCost positive: (A, C) together + self-pair (B, B) -> 2");
    check(h.usedStationsCost == 3, "usedStationsCost positive: stations 0, 1, 3 -> 3");
    check(h.workloadImbalanceCost == 3, "workloadImbalanceCost positive: loads 3 / 5 / 6 -> 3");
    check(sameScore(scoreOf(fixtureH()), Score{-10, -3.0}), "fixture H default score -10hard/-3soft");

    const RawCosts z = rawOf(fixtureZ());
    check(z.stationCapacityCost == 0, "stationCapacityCost zero: loads 5 / 5 on cycle 5");
    check(z.taskPrecedenceCost == 0, "taskPrecedenceCost zero: A, C on 0 before B, D on 1");
    check(z.maxStationsCost == 0, "maxStationsCost zero: every task below station 3");
    check(z.taskStationBoundsCost == 0, "taskStationBoundsCost zero: every task inside its domain");
    check(z.fixedTaskStationsCost == 0, "fixedTaskStationsCost zero: A and C on their fixed 0");
    check(z.forbiddenTaskStationsCost == 0, "forbiddenTaskStationsCost zero: no forbidden station hit");
    check(z.sameStationGroupsCost == 0, "sameStationGroupsCost zero: each group on one station");
    check(z.differentStationPairsCost == 0, "differentStationPairsCost zero: every pair separated");
    check(z.usedStationsCost == 2, "usedStationsCost on Z: 2 stations");
    check(z.workloadImbalanceCost == 0, "workloadImbalanceCost zero: loads 5 / 5");
    check(sameScore(scoreOf(fixtureZ()), Score{0, -2.0}), "fixture Z default score 0hard/-2soft");

    AssemblyLineBalancingProblem empty = fixtureH();
    for (Task &t : empty.tasks)
        t.station = -1;
    const RawCosts e = rawOf(empty);
    check(e.usedStationsCost == 0 && e.workloadImbalanceCost == 0,
          "usedStationsCost / workloadImbalanceCost zero with no task assigned");
    check(sameScore(scoreOf(empty), Score{0, 0.0}),
          "every task unassigned scores 0: unassigned tasks contribute nothing");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Edge cases.
// -------------------------------------------------------------------------
void verifyEdgeCases()
{
    std::printf("== (b) Edge cases ==\n");
    AssemblyLineBalancingProblem none;
    none.cycleTime = 5;
    check(sameScore(scoreOf(none), Score{0, 0.0}), "empty problem scores 0hard/0soft");
    {
        AssemblyLineBalancingProblem p = none;
        ScoreCalculator::prepare(p);
        check(p.stationCount == 0, "empty problem: station count 0");
    }

    // A station far above the task count: V follows it, no limit applies.
    AssemblyLineBalancingProblem far;
    far.cycleTime = 5;
    far.tasks = {task("A", 3, 7), task("B", 3, 0)};
    ScoreCalculator::prepare(far);
    check(far.stationCount == 8, "station 7 of 2 tasks: V = 8");
    check(far.stationLoads().size() == 8 && far.stationLoads()[7] == 3
              && far.stationLoads()[3] == 0 && far.stationsUsed() == 2,
          "station loads list the empty middle stations");
    const RawCosts farRaw = ScoreCalculator::rawCosts(far);
    check(farRaw.maxStationsCost == 0 && farRaw.taskStationBoundsCost == 0,
          "no max_stations / max_station: no station is too far");
    far.maxStations = 2;
    ScoreCalculator::prepare(far);
    check(ScoreCalculator::rawCosts(far).maxStationsCost == 6, "max_stations 2, station 7 -> 7 - 2 + 1");

    // Duplicate arcs count once per copy; a self-arc never costs.
    AssemblyLineBalancingProblem arcs;
    arcs.cycleTime = 10;
    arcs.tasks = {task("A", 1, 2), task("B", 1, 1)};
    arcs.precedences = {{0, 1}, {0, 1}, {1, 1}};
    check(rawOf(arcs).taskPrecedenceCost == 2, "a duplicated reversed arc counts twice, a self-arc 0");

    // Unassigned endpoints never count.
    AssemblyLineBalancingProblem partial = fixtureH();
    partial.tasks[1].station = -1;  // B
    const RawCosts pr = rawOf(partial);
    check(pr.taskPrecedenceCost == 0, "an arc into an unassigned task does not count");
    check(pr.differentStationPairsCost == 1, "pairs with an unassigned task (incl. its self-pair) do not count");
    check(pr.sameStationGroupsCost == 0, "an unassigned member does not split its group");
    check(pr.taskStationBoundsCost == 1, "an unassigned task is outside no domain");

    // Overlapping groups: a task in two groups splits both.
    AssemblyLineBalancingProblem overlap;
    overlap.cycleTime = 10;
    overlap.tasks = {task("A", 1, 0), task("B", 1, 1), task("C", 1, 2)};
    overlap.sameStationGroups = {{"x", {0, 1}}, {"y", {1, 2, 0}}};
    check(rawOf(overlap).sameStationGroupsCost == 3, "groups {A, B} -> 1 and {B, C, A} -> 2");

    // Invalid facts are refused by prepare(); with `message`, the error must
    // name the field exactly.
    auto refuses = [](AssemblyLineBalancingProblem p, const std::string &message = "")
    {
        try
        {
            ScoreCalculator::prepare(p);
        }
        catch (const std::invalid_argument &e)
        {
            return message.empty() || std::string(e.what()) == message;
        }
        return false;
    };
    AssemblyLineBalancingProblem bad = fixtureH();
    bad.tasks[0].duration = 0;
    check(refuses(bad), "duration 0 is refused");
    bad = fixtureH();
    bad.tasks[0].station = -2;
    check(refuses(bad), "station -2 is refused");
    bad = fixtureH();
    bad.precedences.push_back({0, 9});
    check(refuses(bad), "an arc to a missing task is refused");
    bad = fixtureH();
    bad.cycleTime = 0;
    check(refuses(bad), "cycle time 0 is refused");
    // The station-domain rules of the pydantic model, for direct core users.
    bad = fixtureH();
    bad.tasks[1].fixedStation = -3;  // B
    check(refuses(bad, "task 'B': fixed_station must be >= 0"), "fixed_station -3 is refused by name");
    bad.tasks[1].fixedStation = -1;
    check(refuses(bad, "task 'B': fixed_station must be >= 0"), "fixed_station -1 is refused too");
    bad = fixtureH();
    bad.tasks[0].minStation = -1;  // A
    check(refuses(bad, "task 'A': min_station must be >= 0"), "min_station -1 is refused by name");
    bad = fixtureH();
    bad.tasks[3].maxStation = 0;  // D, min_station 0
    check(!refuses(bad), "max_station == min_station is accepted");
    bad.tasks[3].minStation = 1;
    check(refuses(bad, "task 'D': max_station must be >= min_station"),
          "max_station below min_station is refused by name");
    bad = fixtureH();
    bad.maxStations = 0;
    check(refuses(bad, "max_stations must be >= 1"), "max_stations 0 is refused by name");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) The construction, ported from the 0.1.5 package.
// -------------------------------------------------------------------------
AssemblyLineBalancingProblem threeTasks()
{
    AssemblyLineBalancingProblem p;
    p.cycleTime = 5;
    p.tasks = {task("A", 3), task("B", 3), task("C", 2)};
    p.precedences = {{0, 1}};
    return p;
}

Solver zeroBudget()
{
    Solver::Config config;
    config.spentLimit = std::chrono::milliseconds(0);
    return Solver(config);
}

void verifyConstruction()
{
    std::printf("== (c) Construction ==\n");
    {
        AssemblyLineBalancingProblem p = threeTasks();
        const std::vector<int> order = topologicalOrder(p);
        check(order == std::vector<int>({0, 2, 1}), "Kahn levels sorted by id: [A, C], then [B]");
        zeroBudget().solve(p);
        check(p.assignment() == std::vector<int>({0, 1, 0}), "plain construction: A 0, B 1, C 0");
        check(p.score.hard == 0 && p.score.soft == -2.0, "plain construction scores 0hard/-2soft");
    }
    {
        AssemblyLineBalancingProblem p = threeTasks();
        for (Task &t : p.tasks)
            t.station = 2;
        zeroBudget().solve(p);
        check(p.assignment() == std::vector<int>({0, 1, 0}), "without warm start input stations are ignored");
    }
    {
        AssemblyLineBalancingProblem p = threeTasks();
        p.warmStart = true;
        p.tasks[2].station = 2;  // C seeded at 2
        zeroBudget().solve(p);
        check(p.assignment() == std::vector<int>({0, 2, 2}), "partial seed C 2: A 0, B 2, C 2");
        check(!p.warmStart, "the seed is used once (warmStart reset)");
    }
    {
        AssemblyLineBalancingProblem p;
        p.cycleTime = 5;
        p.tasks = {task("A", 1, -1, 0, std::nullopt, 1), task("B", 1, 0)};
        p.warmStart = true;
        zeroBudget().solve(p);
        check(p.assignment() == std::vector<int>({1, 0}), "an unseeded fixed task goes to its fixed station");
    }
    {
        AssemblyLineBalancingProblem p = threeTasks();
        p.maxStations = 2;
        p.warmStart = true;
        p.tasks[0].station = 2;  // A seeded at max_stations
        zeroBudget().solve(p);
        check(p.assignment() == std::vector<int>({2, 3, 2}), "seed A 2: C joins it, B overflows to 3");
        check(p.stationCount == 4, "V taken after the construction: 4 for 3 tasks");
        check(p.score.hard < 0, "beyond max_stations stays a hard penalty");
    }
    {
        AssemblyLineBalancingProblem p = threeTasks();
        p.warmStart = true;
        p.tasks = {task("A", 3, 0), task("B", 2, 0)};
        p.tasks[0].fixedStation = 1;
        p.precedences.clear();
        zeroBudget().solve(p);
        check(p.assignment() == std::vector<int>({0, 0}) && p.score.hard == -1,
              "a complete seed comes back unchanged, its broken fixed station a hard penalty");
    }
    {
        // Refused before the construction copies the fixed station into
        // Task::station, so the message names the field.
        AssemblyLineBalancingProblem p = threeTasks();
        p.tasks[0].fixedStation = -3;
        bool named = false;
        try
        {
            zeroBudget().solve(p);
        }
        catch (const std::invalid_argument &e)
        {
            named = std::string(e.what()) == "task 'A': fixed_station must be >= 0";
        }
        check(named, "solve() refuses fixed_station -3 by name");
    }
    auto throwsCycle = [](AssemblyLineBalancingProblem p)
    {
        try
        {
            zeroBudget().solve(p);
        }
        catch (const std::invalid_argument &e)
        {
            return std::string(e.what()) == "precedence graph must be acyclic";
        }
        return false;
    };
    AssemblyLineBalancingProblem cyclic = threeTasks();
    cyclic.precedences.push_back({1, 0});
    check(throwsCycle(cyclic), "a cycle is refused");
    AssemblyLineBalancingProblem selfArc = threeTasks();
    selfArc.precedences.push_back({2, 2});
    check(throwsCycle(selfArc), "a self-arc is refused by the construction");
    AssemblyLineBalancingProblem seededCycle = cyclic;
    seededCycle.warmStart = true;
    for (Task &t : seededCycle.tasks)
        t.station = 0;
    check(throwsCycle(seededCycle), "a cycle is refused even for a complete seed");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Incremental == full recompute over a random walk.
// -------------------------------------------------------------------------
// Returns the number of states whose score differs; `totalMismatches`
// counts the states where some constraint's maintained raw total differs
// from ScoreCalculator::rawCosts, `nonZeroStates` the states per constraint
// with a non-zero raw total.
int randomWalk(AssemblyLineBalancingProblem &p,
               AssemblyLineBalancingIncrementalScoreCalculator &calculator,
               int steps,
               std::uint64_t seed,
               int &totalMismatches,
               std::map<std::string, int> &nonZeroStates)
{
    optimaru::ScoreDirector<AssemblyLineBalancingProblem> director(p, calculator);
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const int n = p.taskCount();
    const int values = p.stationCount;
    int mismatches = 0;
    int printBudget = 3;
    for (int step = 0; step < steps; ++step)
    {
        const double kind = unit(rng);
        if (kind < 0.55)
        {
            const int t = static_cast<int>(rng() % n);
            const int value = unit(rng) < 0.1 ? -1 : static_cast<int>(rng() % values);
            director.changeVariable(t, value);
        }
        else if (kind < 0.85)
        {
            const int a = static_cast<int>(rng() % n);
            const int b = static_cast<int>(rng() % n);
            const int va = p.tasks[a].station;
            const int vb = p.tasks[b].station;
            director.beginVariableChanges();
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
            director.endVariableChanges();
        }
        else
        {
            // CloseStationMove-like batch: empty one station.
            const int victim = static_cast<int>(rng() % values);
            director.beginVariableChanges();
            for (int t = 0; t < n; ++t)
            {
                if (p.tasks[t].station == victim)
                    director.changeVariable(t, static_cast<int>(rng() % values));
            }
            director.endVariableChanges();
        }
        AssemblyLineBalancingProblem shadow = p;
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = director.calculateScore();
        if (!sameScore(full, incremental) && ++mismatches <= 3)
        {
            std::printf("  MISMATCH @step %d: incremental=%s full=%s\n",
                        step,
                        incremental.toString().c_str(),
                        full.toString().c_str());
        }
        if (!sameTotals(namedTotals(calculator.rawCosts()), namedTotals(ScoreCalculator::rawCosts(shadow)),
                        nonZeroStates, "@step " + std::to_string(step), printBudget))
            ++totalMismatches;
    }
    return mismatches;
}

void verifyIncremental()
{
    std::printf("== (d) Incremental == full recompute ==\n");
    int mismatches = 0;
    int totalMismatches = 0;
    std::map<std::string, int> nonZeroStates;
    for (std::uint64_t seed = 1; seed <= 6; ++seed)
    {
        AssemblyLineBalancingProblem p = randomInstance(12 + static_cast<int>(seed), seed);
        ScoreCalculator::prepare(p);
        AssemblyLineBalancingIncrementalScoreCalculator calculator;
        mismatches += randomWalk(p, calculator, 600, 100 + seed, totalMismatches, nonZeroStates);
    }
    check(mismatches == 0, "3600 random change / swap / unassign / batch moves, incremental == full");
    check(totalMismatches == 0, "after every move each constraint's maintained raw total == rawCosts()");
    // workloadImbalanceCost (default weight 0) is not in the monolithic
    // score; rawCosts() still reads it from the maintained loads, so it is
    // compared here too.
    check(everyConstraintNonZero(nonZeroStates, kNames), "every constraint is non-zero at least once in the walk");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) FULL_ASSERT solves.
// -------------------------------------------------------------------------
bool fullAssertSolve(AssemblyLineBalancingProblem p, const std::string &label)
{
    Solver::Config config;
    config.spentLimit = std::chrono::seconds(30);
    config.stepCountLimit = 1500;
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    config.ruinRecreateUnimprovedStepTrigger = 100;
    config.ruinRecreatePolishSteps = 50;
    config.valueRecommenderProbability = 0.5;
    config.closeStationMoves = true;
    bool corrupted = false;
    try
    {
        Solver(config).solve(p);
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, label + ": FULL_ASSERT solve without score corruption");
    if (corrupted)
        return false;
    AssemblyLineBalancingProblem shadow = p;
    check(sameScore(p.score, ScoreCalculator::calculateScore(shadow)),
          label + ": solver-reported score == full recompute");
    return true;
}

void verifyFullAssertSolve()
{
    std::printf("== (e) FULL_ASSERT solves ==\n");
    AssemblyLineBalancingProblem rich = randomInstance(14, 77);
    rich.precedences.clear();  // the construction needs an acyclic graph
    for (int t = 1; t < rich.taskCount(); t += 2)
        rich.precedences.push_back({t - 1, t});
    fullAssertSolve(rich, "feature-rich instance");
    fullAssertSolve(salbpInstance(30, 5), "SALBP-like instance");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) Determinism, multi-start, quality, zero budget.
// -------------------------------------------------------------------------
void verifySolves()
{
    std::printf("== (f) Solves ==\n");
    Solver::Config config;
    config.spentLimit = std::chrono::seconds(30);
    config.stepCountLimit = 4000;
    config.randomSeed = 11;
    AssemblyLineBalancingProblem a = salbpInstance(40, 9);
    AssemblyLineBalancingProblem b = salbpInstance(40, 9);
    Solver(config).solve(a);
    Solver(config).solve(b);
    check(a.assignment() == b.assignment() && sameScore(a.score, b.score),
          "a seeded step-limited solve is reproducible");

    Solver::Config parallel = config;
    parallel.parallelSolverCount = 3;
    AssemblyLineBalancingProblem c = salbpInstance(40, 9);
    AssemblyLineBalancingProblem d = salbpInstance(40, 9);
    Solver(parallel).solve(c);
    Solver(parallel).solve(d);
    AssemblyLineBalancingProblem shadow = c;
    check(c.assignment() == d.assignment() && sameScore(c.score, d.score),
          "the parallel multi-start is reproducible per seed");
    check(sameScore(c.score, ScoreCalculator::calculateScore(shadow)),
          "the winning worker's score == full recompute");
    check(!(c.score < a.score), "three starts score no worse than worker 0 alone");

    AssemblyLineBalancingProblem big = salbpInstance(100, 21);
    std::int64_t total = 0;
    for (const Task &t : big.tasks)
        total += t.duration;
    Solver::Config timed;
    timed.spentLimit = std::chrono::seconds(2);
    timed.randomSeed = 1;
    Solver(timed).solve(big);
    bool assigned = true;
    for (const Task &t : big.tasks)
        assigned &= t.isAssigned();
    check(assigned && big.score.hard == 0, "a 100-task SALBP-like instance reaches 0hard");
    const std::int64_t lowerBound = (total + big.cycleTime - 1) / big.cycleTime;
    std::printf("  stations used %d, lower bound %lld\n", big.stationsUsed(), static_cast<long long>(lowerBound));
    check(big.stationsUsed() <= lowerBound + 2, "within two stations of the trivial lower bound");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyEdgeCases();
    verifyConstruction();
    verifyIncremental();
    verifyFullAssertSolve();
    verifySolves();

    if (failures == 0)
    {
        std::printf("All assembly-line-balancing checks PASSED.\n");
        return 0;
    }
    std::printf("%d assembly-line-balancing check(s) FAILED.\n", failures);
    return 1;
}

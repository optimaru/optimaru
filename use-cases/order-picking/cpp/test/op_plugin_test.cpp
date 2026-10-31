// Plugin-assembly (op/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 300 random list
//       layouts (unassigned pick tasks included)
//   (b) incremental score == full recompute across 2000 random LIST moves
//       driven through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: each component in isolation, hand-computed
//   (d) weight / isHard overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(solution, calculator) under FULL_ASSERT
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "op/constraints.hpp"
#include "op/demo_data.hpp"
#include "op/domain.hpp"
#include "op/score_calculator.hpp"
#include "op/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace op;

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
                long long expectedHard,
                double expectedSoft)
{
    const bool ok = (actual.hard == expectedHard
                     && std::fabs(actual.soft - expectedSoft) < 1e-9);
    std::printf("  [%s] %-52s got=%s expected=%lldhard/%gsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedSoft);
    if (!ok)
        ++failures;
}

bool sameScore(const Score &a, const Score &b)
{
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

WarehouseLocation loc(char column, int shelvingRow1Based, Side side, int row)
{
    WarehouseLocation location;
    location.column = column - 'A';
    location.shelvingRow = shelvingRow1Based - 1;
    location.side = side;
    location.row = row;
    return location;
}

int addPick(OrderPickingSolution &s,
            int orderIndex,
            int volume,
            const WarehouseLocation &location)
{
    PickTask task;
    task.id = "p" + std::to_string(s.pickTasks.size());
    task.orderIndex = orderIndex;
    task.volume = volume;
    task.location = location;
    s.pickTasks.push_back(task);
    return static_cast<int>(s.pickTasks.size()) - 1;
}

int addTrolley(OrderPickingSolution &s,
               int bucketCount,
               int bucketCapacity,
               const WarehouseLocation &location)
{
    Trolley trolley;
    trolley.id = "t" + std::to_string(s.trolleys.size());
    trolley.bucketCount = bucketCount;
    trolley.bucketCapacity = bucketCapacity;
    trolley.location = location;
    s.trolleys.push_back(trolley);
    return static_cast<int>(s.trolleys.size()) - 1;
}

// A base solution with `orderCount` orders and no picks yet.
OrderPickingSolution makeBase(int orderCount)
{
    OrderPickingSolution s;
    for (int o = 0; o < orderCount; ++o)
        s.orders.push_back({"o" + std::to_string(o)});
    return s;
}

Score scoreWith(DecomposedCalculator &calculator, OrderPickingSolution &s)
{
    calculator.resetWorkingSolution(s);
    return calculator.calculateScore();
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute, on random
// trolley layouts (some picks deliberately left unassigned).
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    OrderPickingSolution s = buildDemoData();

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const int taskCount = static_cast<int>(s.pickTasks.size());
    const int trolleyCount = static_cast<int>(s.trolleys.size());
    int mismatches = 0;
    const int rounds = 300;
    for (int round = 0; round < rounds; ++round)
    {
        for (Trolley &trolley : s.trolleys)
            trolley.pickTaskIndices.clear();
        for (int t = 0; t < taskCount; ++t)
        {
            if (unit(rng) < 0.10)
                continue;  // left unassigned
            const int trolley = static_cast<int>(rng() % trolleyCount);
            s.trolleys[trolley].pickTaskIndices.push_back(t);
        }
        const Score plugin = scoreWith(calculator, s);
        const Score monolithic = ScoreCalculator::calculateScore(s);
        if (!sameScore(plugin, monolithic))
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
          std::to_string(rounds) + " random list layouts score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of LIST
// mutations, driven through the ScoreDirector.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    OrderPickingSolution s = buildDemoData();
    const int taskCount = static_cast<int>(s.pickTasks.size());
    const int trolleyCount = static_cast<int>(s.trolleys.size());

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<OrderPickingSolution> director(s, calculator);

    std::mt19937_64 rng(2024);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    std::vector<int> taskList(taskCount, -1);
    std::vector<int> unassigned;
    for (int t = 0; t < taskCount; ++t)
        unassigned.push_back(t);

    auto indexWithin = [&](int trolley, int task)
    {
        const std::vector<int> &picks = s.trolleys[trolley].pickTaskIndices;
        for (std::size_t p = 0; p < picks.size(); ++p)
        {
            if (picks[p] == task)
                return static_cast<int>(p);
        }
        return -1;
    };

    int mismatches = 0;
    const int steps = 2000;
    for (int step = 0; step < steps; ++step)
    {
        const double dice = unit(rng);
        if (dice < 0.35 && !unassigned.empty())
        {
            const std::size_t pick = rng() % unassigned.size();
            const int task = unassigned[pick];
            unassigned.erase(unassigned.begin()
                             + static_cast<std::ptrdiff_t>(pick));
            const int trolley = static_cast<int>(rng() % trolleyCount);
            const int position = static_cast<int>(
                rng() % (s.trolleys[trolley].pickTaskIndices.size() + 1));
            director.insertListElement(trolley, position, task);
            taskList[task] = trolley;
        }
        else if (dice < 0.55)
        {
            const int task = static_cast<int>(rng() % taskCount);
            if (taskList[task] < 0)
                continue;
            director.removeListElement(taskList[task],
                                       indexWithin(taskList[task], task));
            taskList[task] = -1;
            unassigned.push_back(task);
        }
        else if (dice < 0.85)
        {
            const int task = static_cast<int>(rng() % taskCount);
            if (taskList[task] < 0)
                continue;
            director.removeListElement(taskList[task],
                                       indexWithin(taskList[task], task));
            const int trolley = static_cast<int>(rng() % trolleyCount);
            const int position = static_cast<int>(
                rng() % (s.trolleys[trolley].pickTaskIndices.size() + 1));
            director.insertListElement(trolley, position, task);
            taskList[task] = trolley;
        }
        else
        {
            const int trolley = static_cast<int>(rng() % trolleyCount);
            const int size
                = static_cast<int>(s.trolleys[trolley].pickTaskIndices.size());
            if (size < 2)
                continue;
            int first = static_cast<int>(rng() % size);
            int last = static_cast<int>(rng() % size);
            if (first > last)
                std::swap(first, last);
            director.reverseListSegment(trolley, first, last);
        }

        const Score incremental = director.calculateScore();
        const Score full = ScoreCalculator::calculateScore(s);
        if (!sameScore(full, incremental))
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
// (c) Single-component assemblies on the Java provider test's data.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Single-component assemblies ==\n");
    const WarehouseLocation origin = loc('A', 1, Side::LEFT, 0);

    {  // requiredNumberOfBuckets, the Java test's exact data: trolley1
        // (2 buckets, capacity 5) needs 3, trolley2 (2 buckets, capacity 10)
        // needs 6 -> 1 + 4 short.
        OrderPickingSolution s = makeBase(2);
        const int t1 = addTrolley(s, 2, 5, origin);
        const int t2 = addTrolley(s, 2, 10, origin);
        s.trolleys[t1].pickTaskIndices = {addPick(s, 0, 4, origin),
                                          addPick(s, 0, 5, origin),
                                          addPick(s, 1, 4, origin),
                                          addPick(s, 1, 1, origin)};
        s.trolleys[t2].pickTaskIndices = {addPick(s, 0, 9, origin),
                                          addPick(s, 0, 8, origin),
                                          addPick(s, 1, 8, origin),
                                          addPick(s, 1, 6, origin),
                                          addPick(s, 1, 10, origin),
                                          addPick(s, 1, 9, origin)};
        DecomposedCalculator calculator;
        calculator.emplace<RequiredNumberOfBuckets>();
        checkScore(
            "requiredNumberOfBuckets alone", scoreWith(calculator, s), -5, 0.0);
    }
    {  // minimizeDistanceFromPreviousPickTask alone: origin -> A1L2 -> A1L5.
        OrderPickingSolution s = makeBase(1);
        const int t = addTrolley(s, 1, 100, origin);
        const WarehouseLocation first = loc('A', 1, Side::LEFT, 2);
        const WarehouseLocation second = loc('A', 1, Side::LEFT, 5);
        s.trolleys[t].pickTaskIndices
            = {addPick(s, 0, 1, first), addPick(s, 0, 1, second)};
        const double expected = calculateDistance(origin, first)
                                + calculateDistance(first, second);
        DecomposedCalculator calculator;
        calculator.emplace<MinimizeDistanceFromPreviousPickTask>();
        checkScore("minimizeDistanceFromPreviousPickTask alone",
                   scoreWith(calculator, s),
                   0,
                   -expected);
    }
    {  // minimizeDistanceFromLastPickTaskToPathOrigin alone: only the walk
        // back from the final pick counts.
        OrderPickingSolution s = makeBase(1);
        const int t = addTrolley(s, 1, 100, origin);
        const WarehouseLocation first = loc('A', 1, Side::LEFT, 2);
        const WarehouseLocation second = loc('A', 1, Side::LEFT, 5);
        s.trolleys[t].pickTaskIndices
            = {addPick(s, 0, 1, first), addPick(s, 0, 1, second)};
        DecomposedCalculator calculator;
        calculator.emplace<MinimizeDistanceFromLastPickTaskToPathOrigin>();
        checkScore("minimizeDistanceFromLastPickTaskToPathOrigin alone",
                   scoreWith(calculator, s),
                   0,
                   -static_cast<double>(calculateDistance(second, origin)));
    }
    {  // minimizeOrderSplitByTrolley alone: one order on two trolleys and a
        // second order on one -> three (order, trolley) pairs.
        OrderPickingSolution s = makeBase(2);
        const int t1 = addTrolley(s, 10, 100, origin);
        const int t2 = addTrolley(s, 10, 100, origin);
        s.trolleys[t1].pickTaskIndices
            = {addPick(s, 0, 1, origin), addPick(s, 1, 1, origin)};
        s.trolleys[t2].pickTaskIndices = {addPick(s, 0, 1, origin)};
        DecomposedCalculator calculator;
        calculator.emplace<MinimizeOrderSplitByTrolley>();
        checkScore("minimizeOrderSplitByTrolley alone",
                   scoreWith(calculator, s),
                   0,
                   -3000.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    const WarehouseLocation origin = loc('A', 1, Side::LEFT, 0);
    OrderPickingSolution s = makeBase(1);
    const int t = addTrolley(s, 1, 5, origin);
    s.trolleys[t].pickTaskIndices = {addPick(s, 0, 5, origin),
                                     addPick(s, 0, 5, origin),
                                     addPick(s, 0, 5, origin)};
    // 15 volume / capacity 5 = 3 buckets against 1 -> 2 short.

    {
        DecomposedCalculator calculator;
        calculator.emplace<RequiredNumberOfBuckets>(10.0);
        checkScore("requiredNumberOfBuckets re-weighted to 10",
                   scoreWith(calculator, s),
                   -20,
                   0.0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<RequiredNumberOfBuckets>();
        component->isHard = false;
        calculator.add(std::move(component));
        checkScore("requiredNumberOfBuckets demoted to soft",
                   scoreWith(calculator, s),
                   0,
                   -2.0);
    }
    {
        // exponent 2 shapes the raw shortage (2) before the weight.
        DecomposedCalculator calculator;
        auto component = std::make_unique<RequiredNumberOfBuckets>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("requiredNumberOfBuckets with exponent 2",
                   scoreWith(calculator, s),
                   -4,
                   0.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    OrderPickingSolution s = buildDemoData();
    std::mt19937_64 rng(5);
    for (int t = 0; t < static_cast<int>(s.pickTasks.size()); ++t)
    {
        const int trolley = static_cast<int>(rng() % s.trolleys.size());
        s.trolleys[trolley].pickTaskIndices.push_back(t);
    }

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    const Score score = scoreWith(calculator, s);

    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-6,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == 4,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    OrderPickingSolution s = buildDemoData(3, 4, 60 * 40 * 20, 3, 37);

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
        solver.solve(s, calculator);
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(s);
        check(sameScore(s.score, recomputed),
              "plugin-reported score == monolithic full recompute");
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

    if (failures == 0)
    {
        std::printf("All order-picking plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d order-picking plugin check(s) FAILED.\n", failures);
    return 1;
}

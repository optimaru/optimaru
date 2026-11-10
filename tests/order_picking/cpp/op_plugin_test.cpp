// Plugin-assembly (op/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedObjective
//       reproduces the monolithic score bit-for-bit on 300 random list
//       layouts (unassigned pick tasks included)
//   (b) incremental score == full recompute across 2000 random LIST moves
//       driven through the engine's Editor protocol (plugin path)
//   (c) partial assemblies: each component in isolation, hand-computed
//   (d) weight / level overrides
//   (e) constraint_totals() explains exactly the reported score
//   (f) a 2-second solve(solution, calculator) under ScoreChecks::Full
//   (g) the defaultConstraints() builder: named edits == by-hand assembly
//   (h) each component's raw cost, hand-calculated: a positive and a zero
//       case per constraint (raw 0 = no violation; orderSplitByTrolleyCost
//       counts every (order, trolley) pair, so its zero case has no picks)
//   (i) the catalog: info() off the default assembly == the components'
//       kDefaultWeight / kDefaultLevel; the level/weight guards
//   (j) repeated components and the offset / clip knobs: a hand-calculated
//       repeat (walk + 0.5 x walk^2), threshold / literal / negative offsets
//       by hand (an empty trolley counts in literal mode), per-trolley hard
//       rounding, the literal formula refused on hard at reset and kept on
//       soft, incremental == a fresh recompute over a random walk, every
//       preview == the applied score under ScoreChecks::Full and a ScoreChecks::Full
//       solve with repeated, shifted components
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "op/constraints.hpp"
#include "op/demo_data.hpp"
#include "op/domain.hpp"
#include "op/score_calculator.hpp"
#include "op/solver.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/testing/preview_parity.hpp"

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
                actual.str().c_str(),
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
    calculator.rebuild(s);
    return calculator.score();
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
        const Score monolithic = ScoreCalculator::score(s);
        if (!sameScore(plugin, monolithic))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n",
                            round,
                            plugin.str().c_str(),
                            monolithic.str().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(rounds) + " random list layouts score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of LIST
// mutations, driven through the Editor.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    OrderPickingSolution s = buildDemoData();
    const int taskCount = static_cast<int>(s.pickTasks.size());
    const int trolleyCount = static_cast<int>(s.trolleys.size());

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::Editor<OrderPickingSolution> editor(s, calculator);

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
            editor.insert(trolley, position, task);
            taskList[task] = trolley;
        }
        else if (dice < 0.55)
        {
            const int task = static_cast<int>(rng() % taskCount);
            if (taskList[task] < 0)
                continue;
            editor.remove(taskList[task],
                                       indexWithin(taskList[task], task));
            taskList[task] = -1;
            unassigned.push_back(task);
        }
        else if (dice < 0.85)
        {
            const int task = static_cast<int>(rng() % taskCount);
            if (taskList[task] < 0)
                continue;
            editor.remove(taskList[task],
                                       indexWithin(taskList[task], task));
            const int trolley = static_cast<int>(rng() % trolleyCount);
            const int position = static_cast<int>(
                rng() % (s.trolleys[trolley].pickTaskIndices.size() + 1));
            editor.insert(trolley, position, task);
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
            editor.reverse(trolley, first, last);
        }

        const Score incremental = editor.score();
        const Score full = ScoreCalculator::score(s);
        if (!sameScore(full, incremental))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n",
                            step,
                            incremental.str().c_str(),
                            full.str().c_str());
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

    {  // trolleyCapacityCost, the Java test's exact data: trolley1
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
        calculator.emplace<TrolleyCapacityCost>();
        checkScore(
            "trolleyCapacityCost alone", scoreWith(calculator, s), -5, 0.0);
    }
    {  // distanceFromPreviousPickTaskCost alone: origin -> A1L2 -> A1L5.
        OrderPickingSolution s = makeBase(1);
        const int t = addTrolley(s, 1, 100, origin);
        const WarehouseLocation first = loc('A', 1, Side::LEFT, 2);
        const WarehouseLocation second = loc('A', 1, Side::LEFT, 5);
        s.trolleys[t].pickTaskIndices
            = {addPick(s, 0, 1, first), addPick(s, 0, 1, second)};
        const double expected = calculateDistance(origin, first)
                                + calculateDistance(first, second);
        DecomposedCalculator calculator;
        calculator.emplace<DistanceFromPreviousPickTaskCost>();
        checkScore("distanceFromPreviousPickTaskCost alone",
                   scoreWith(calculator, s),
                   0,
                   -expected);
    }
    {  // distanceFromLastPickTaskToPathOriginCost alone: only the walk
        // back from the final pick counts.
        OrderPickingSolution s = makeBase(1);
        const int t = addTrolley(s, 1, 100, origin);
        const WarehouseLocation first = loc('A', 1, Side::LEFT, 2);
        const WarehouseLocation second = loc('A', 1, Side::LEFT, 5);
        s.trolleys[t].pickTaskIndices
            = {addPick(s, 0, 1, first), addPick(s, 0, 1, second)};
        DecomposedCalculator calculator;
        calculator.emplace<DistanceFromLastPickTaskToPathOriginCost>();
        checkScore("distanceFromLastPickTaskToPathOriginCost alone",
                   scoreWith(calculator, s),
                   0,
                   -static_cast<double>(calculateDistance(second, origin)));
    }
    {  // orderSplitByTrolleyCost alone: one order on two trolleys and a
        // second order on one -> three (order, trolley) pairs.
        OrderPickingSolution s = makeBase(2);
        const int t1 = addTrolley(s, 10, 100, origin);
        const int t2 = addTrolley(s, 10, 100, origin);
        s.trolleys[t1].pickTaskIndices
            = {addPick(s, 0, 1, origin), addPick(s, 1, 1, origin)};
        s.trolleys[t2].pickTaskIndices = {addPick(s, 0, 1, origin)};
        DecomposedCalculator calculator;
        calculator.emplace<OrderSplitByTrolleyCost>();
        checkScore("orderSplitByTrolleyCost alone",
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
        calculator.emplace<TrolleyCapacityCost>(10.0);
        checkScore("trolleyCapacityCost re-weighted to 10",
                   scoreWith(calculator, s),
                   -20,
                   0.0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<TrolleyCapacityCost>();
        component->level = optimaru::Level::Soft;
        calculator.add(std::move(component));
        checkScore("trolleyCapacityCost demoted to soft",
                   scoreWith(calculator, s),
                   0,
                   -2.0);
    }
    {
        // exponent 2 shapes the raw shortage (2) before the weight.
        DecomposedCalculator calculator;
        auto component = std::make_unique<TrolleyCapacityCost>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("trolleyCapacityCost with exponent 2",
                   scoreWith(calculator, s),
                   -4,
                   0.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraint_totals(): the per-constraint breakdown adds up.
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
    for (const optimaru::ConstraintTotal &total : calculator.constraint_totals())
        (total.is_hard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-6,
          "per-constraint totals sum to the reported score");
    check(calculator.constraint_totals().size() == 4,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) ScoreChecks::Full solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    OrderPickingSolution s = buildDemoData(3, 4, 60 * 40 * 20, 3, 37);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(2);
    config.seed = 3;
    config.score_checks = optimaru::ScoreChecks::Full;
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
    catch (const optimaru::ScoreMismatchError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::score(s);
        check(sameScore(s.score, recomputed),
              "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) The defaultConstraints() builder: named edits by registry name.
// -------------------------------------------------------------------------
void verifyAssemblyBuilder()
{
    std::printf("== (g) defaultConstraints() builder ==\n");
    OrderPickingSolution s = buildDemoData();
    std::mt19937_64 rng(17);
    for (int t = 0; t < static_cast<int>(s.pickTasks.size()); ++t)
    {
        const int trolley = static_cast<int>(rng() % s.trolleys.size());
        s.trolleys[trolley].pickTaskIndices.push_back(t);
    }

    // By hand: capacity demoted to soft, the split re-weighted to 100, the
    // return leg dropped.
    DecomposedCalculator byHand;
    {
        auto capacity = std::make_unique<TrolleyCapacityCost>();
        capacity->level = optimaru::Level::Soft;
        byHand.add(std::move(capacity));
        byHand.emplace<DistanceFromPreviousPickTaskCost>();
        byHand.emplace<OrderSplitByTrolleyCost>(100.0);
    }
    const Score expected = scoreWith(byHand, s);

    // By name: same three edits, one expression.
    DecomposedCalculator built;
    defaultConstraints()
        .soft("trolleyCapacityCost")
        .weight("orderSplitByTrolleyCost", 100)
        .drop("distanceFromLastPickTaskToPathOriginCost")
        .into(built);
    check(sameScore(scoreWith(built, s), expected),
          "named edits == the same assembly listed by hand");

    // The default assembly, untouched, still matches the monolith.
    DecomposedCalculator plain;
    defaultConstraints().into(plain);
    check(sameScore(scoreWith(plain, s), ScoreCalculator::score(s)),
          "defaultConstraints() alone == the monolithic score");

    // A typo must not quietly leave the cost function as it was.
    bool threw = false;
    try
    {
        defaultConstraints().weight("orderSplitByTrolleysCost", 4);
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    check(threw,
          "a misspelled constraint name throws instead of doing nothing");

    // only() isolates a subset: just the split rule, nothing else scored.
    // Expected by hand: -1000 per distinct (order, trolley) pair.
    long long pairs = 0;
    for (const Trolley &trolley : s.trolleys)
    {
        std::set<int> orders;
        for (int task : trolley.pickTaskIndices)
            orders.insert(s.pickTasks[task].orderIndex);
        pairs += static_cast<long long>(orders.size());
    }
    DecomposedCalculator isolated;
    defaultConstraints().only({"orderSplitByTrolleyCost"}).into(isolated);
    checkScore("only() scores exactly the named constraint",
               scoreWith(isolated, s),
               0,
               -1000.0 * static_cast<double>(pairs));
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) Each component's raw cost on hand-built instances: one violating
// (positive) case and one clean (zero) case per constraint, read through
// constraint_costs() — the raw per-trolley numbers before weight and level.
// Same-shelving, same-side distances are the row difference (op_test (a)).
// -------------------------------------------------------------------------
std::vector<double> rawCosts(OrderPickingSolution &solution,
                             const std::string &name)
{
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(solution);
    calculator.score();
    for (const auto &entry : calculator.constraint_costs())
    {
        if (entry.name == name)
            return entry.unit_costs;
    }
    return {};
}

void verifyRawCostsByHand()
{
    std::printf("== (h) Raw costs by hand, positive and zero ==\n");
    const WarehouseLocation origin = loc('A', 1, Side::LEFT, 0);
    const WarehouseLocation row2 = loc('A', 1, Side::LEFT, 2);
    const WarehouseLocation row5 = loc('A', 1, Side::LEFT, 5);
    {
        // The Java case: trolley1 (2 buckets of 5) carries order1 9 -> 2
        // buckets and order2 5 -> 1, so 3 - 2 = 1 short; trolley2 (2 of 10)
        // carries order1 17 -> 2 and order2 33 -> 4, so 6 - 2 = 4 short.
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
        check(rawCosts(s, "trolleyCapacityCost")
                  == std::vector<double>{1.0, 4.0},
              "trolleyCapacityCost: [1, 4] (3 on 2 buckets; 6 on 2)");
    }
    {
        // Exactly enough: order0 10 -> 1 bucket, order1 7 -> 1 bucket on a
        // 2-bucket trolley of capacity 10; the second trolley is empty.
        OrderPickingSolution s = makeBase(2);
        const int t1 = addTrolley(s, 2, 10, origin);
        addTrolley(s, 1, 10, origin);
        s.trolleys[t1].pickTaskIndices = {addPick(s, 0, 6, origin),
                                          addPick(s, 0, 4, origin),
                                          addPick(s, 1, 7, origin)};
        check(rawCosts(s, "trolleyCapacityCost")
                  == std::vector<double>{0.0, 0.0},
              "trolleyCapacityCost: [0, 0] exactly at bucket count");
    }
    {
        // origin -> A1L2 (2) -> A1L5 (3) on the first trolley; the second is
        // empty.
        OrderPickingSolution s = makeBase(1);
        const int t1 = addTrolley(s, 1, 100, origin);
        addTrolley(s, 1, 100, origin);
        s.trolleys[t1].pickTaskIndices
            = {addPick(s, 0, 1, row2), addPick(s, 0, 1, row5)};
        check(rawCosts(s, "distanceFromPreviousPickTaskCost")
                  == std::vector<double>{5.0, 0.0},
              "distanceFromPreviousPickTaskCost: [5, 0] (2 + 3; empty)");
        // ... and the walk back A1L5 -> origin is 5.
        check(rawCosts(s, "distanceFromLastPickTaskToPathOriginCost")
                  == std::vector<double>{5.0, 0.0},
              "distanceFromLastPickTaskToPathOriginCost: [5, 0] (5; empty)");
    }
    {
        // Every pick on the trolley's own parking spot: no outbound walk.
        OrderPickingSolution s = makeBase(1);
        const int t = addTrolley(s, 1, 100, origin);
        s.trolleys[t].pickTaskIndices
            = {addPick(s, 0, 1, origin), addPick(s, 0, 1, origin)};
        check(rawCosts(s, "distanceFromPreviousPickTaskCost")
                  == std::vector<double>{0.0},
              "distanceFromPreviousPickTaskCost: [0] picks on the spot");
    }
    {
        // origin -> A1L2 -> origin: the last pick is back on the parking
        // spot, so the return leg is 0 (the outbound legs are 2 + 2).
        OrderPickingSolution s = makeBase(1);
        const int t = addTrolley(s, 1, 100, origin);
        s.trolleys[t].pickTaskIndices
            = {addPick(s, 0, 1, row2), addPick(s, 0, 1, origin)};
        check(rawCosts(s, "distanceFromLastPickTaskToPathOriginCost")
                  == std::vector<double>{0.0},
              "distanceFromLastPickTaskToPathOriginCost: [0] last pick at "
              "the origin");
        check(rawCosts(s, "distanceFromPreviousPickTaskCost")
                  == std::vector<double>{4.0},
              "... while distanceFromPreviousPickTaskCost is 2 + 2");
    }
    {
        // order0 on both trolleys, order1 on the first: trolley1 carries 2
        // distinct orders, trolley2 one.
        OrderPickingSolution s = makeBase(2);
        const int t1 = addTrolley(s, 10, 100, origin);
        const int t2 = addTrolley(s, 10, 100, origin);
        s.trolleys[t1].pickTaskIndices = {addPick(s, 0, 1, origin),
                                          addPick(s, 1, 1, origin),
                                          addPick(s, 0, 1, origin)};
        s.trolleys[t2].pickTaskIndices = {addPick(s, 0, 1, origin)};
        check(rawCosts(s, "orderSplitByTrolleyCost")
                  == std::vector<double>{2.0, 1.0},
              "orderSplitByTrolleyCost: [2, 1] (order, trolley) pairs");
    }
    {
        // No assigned pick: no (order, trolley) pair. (An unsplit order
        // still costs 1 — the quickstart's definition, kept.)
        OrderPickingSolution s = makeBase(1);
        addTrolley(s, 1, 100, origin);
        addTrolley(s, 1, 100, origin);
        addPick(s, 0, 1, row2);  // in no list
        check(rawCosts(s, "orderSplitByTrolleyCost")
                  == std::vector<double>{0.0, 0.0},
              "orderSplitByTrolleyCost: [0, 0] without assigned picks");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The catalog read off the default assembly is exactly the components'
// constants, and the level/weight guards hold on the domain's assembly.
// -------------------------------------------------------------------------
bool throwsInvalid(const std::function<void()> &edit)
{
    try
    {
        edit();
    }
    catch (const std::invalid_argument &)
    {
        return true;
    }
    return false;
}

template <typename Component>
bool rowIs(const optimaru::ConstraintInfo &row,
           const char *name,
           optimaru::Level level,
           double weight)
{
    return row.name == name && row.default_level == Component::kDefaultLevel
           && row.default_weight == Component::kDefaultWeight
           && row.default_level == level && row.default_weight == weight
           && row.level_flippable && !row.reward && !row.is_global
           && row.default_enabled;
}

void verifyCatalog()
{
    std::printf("== (i) Catalog and guards ==\n");
    const auto rows = defaultConstraints().info();
    check(rows.size() == 4
              && rowIs<TrolleyCapacityCost>(
                  rows[0], "trolleyCapacityCost", optimaru::Level::Hard, 1.0)
              && rowIs<DistanceFromPreviousPickTaskCost>(
                  rows[1],
                  "distanceFromPreviousPickTaskCost",
                  optimaru::Level::Soft,
                  1.0)
              && rowIs<DistanceFromLastPickTaskToPathOriginCost>(
                  rows[2],
                  "distanceFromLastPickTaskToPathOriginCost",
                  optimaru::Level::Soft,
                  1.0)
              && rowIs<OrderSplitByTrolleyCost>(rows[3],
                                                "orderSplitByTrolleyCost",
                                                optimaru::Level::Soft,
                                                1000.0),
          "info(): capacity hard 1, two distances soft 1, split soft 1000; "
          "all flippable, no reward, no global, all default-enabled");
    check(!throwsInvalid(
              [] { defaultConstraints().soft("trolleyCapacityCost"); })
              && !throwsInvalid(
                  [] { defaultConstraints().hard("orderSplitByTrolleyCost"); }),
          "every level flips both ways");
    check(throwsInvalid(
              []
              {
                  defaultConstraints().weight("orderSplitByTrolleyCost",
                                              -1.0);
              }),
          "a negative weight is refused");
    check(throwsInvalid(
              []
              { defaultConstraints().exponent("trolleyCapacityCost", 0.0); }),
          "a non-positive exponent is refused");
    std::printf("\n");
}


// -------------------------------------------------------------------------
// (j) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with add_instances(), as the Python bindings
// build it from a pick list.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string &name)
{
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// Two orders, three trolleys parked at A1L0: t0 (2 buckets of 10) picks
// order 0 at A1L2 (6) then order 1 at A1L5 (7); t1 (1 bucket of 10) picks
// order 0 at A1L5 (15, 2 buckets); t2 stays empty. Raw per trolley:
// walk [5, 5, 0], walk back [5, 5, 0], orders [2, 1, 0], buckets short
// [0, 1, 0].
OrderPickingSolution repeatInstance()
{
    const WarehouseLocation origin = loc('A', 1, Side::LEFT, 0);
    OrderPickingSolution s = makeBase(2);
    const int t0 = addTrolley(s, 2, 10, origin);
    const int t1 = addTrolley(s, 1, 10, origin);
    addTrolley(s, 1, 10, origin);
    s.trolleys[t0].pickTaskIndices
        = {addPick(s, 0, 6, loc('A', 1, Side::LEFT, 2)), addPick(s, 1, 7, loc('A', 1, Side::LEFT, 5))};
    s.trolleys[t1].pickTaskIndices = {addPick(s, 0, 15, loc('A', 1, Side::LEFT, 5))};
    return s;
}

Score repeatScore(ConstraintAssembly assembly)
{
    OrderPickingSolution s = repeatInstance();
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    return scoreWith(calculator, s);
}

ConstraintAssembly repeatedAssembly()
{
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("distanceFromPreviousPickTaskCost", 40.0)
        .clip("distanceFromPreviousPickTaskCost", false);
    assembly.offset("orderSplitByTrolleyCost", 1.0);
    assembly.add_instances(std::move(copyOf("distanceFromPreviousPickTaskCost")
                                        .weight("distanceFromPreviousPickTaskCost", 0.01)
                                        .exponent("distanceFromPreviousPickTaskCost", 2.0)
                                        .offset("distanceFromPreviousPickTaskCost", 20.0)));
    // The literal bucket copy sits on soft: hard refuses the literal formula
    // with an offset (hard 0 must mean no violation).
    assembly.add_instances(std::move(copyOf("trolleyCapacityCost")
                                        .soft("trolleyCapacityCost")
                                        .weight("trolleyCapacityCost", 2.0)
                                        .offset("trolleyCapacityCost", 1.0)
                                        .clip("trolleyCapacityCost", false)));
    assembly.add_instances(std::move(copyOf("trolleyCapacityCost")
                                        .weight("trolleyCapacityCost", 2.0)
                                        .offset("trolleyCapacityCost", 1.0)));
    assembly.add_instances(std::move(copyOf("orderSplitByTrolleyCost")
                                        .weight("orderSplitByTrolleyCost", 300.0)
                                        .offset("orderSplitByTrolleyCost", -2.0)
                                        .clip("orderSplitByTrolleyCost", false)));
    assembly.add_instances(std::move(copyOf("distanceFromLastPickTaskToPathOriginCost")
                                        .weight("distanceFromLastPickTaskToPathOriginCost", 0.5)
                                        .offset("distanceFromLastPickTaskToPathOriginCost", -3.0)));
    return assembly;
}

// The demo data with every pick task on a trolley, round robin.
OrderPickingSolution assignedDemo(int trolleys, std::uint64_t seed)
{
    OrderPickingSolution s = buildDemoData(trolleys, 4, 60 * 40 * 20, 4, static_cast<std::int64_t>(seed));
    for (Trolley &trolley : s.trolleys)
        trolley.pickTaskIndices.clear();
    for (int task = 0; task < static_cast<int>(s.pickTasks.size()); ++task)
        s.trolleys[static_cast<std::size_t>(task) % s.trolleys.size()].pickTaskIndices.push_back(task);
    return s;
}

void verifyRepeatsAndOffsets()
{
    std::printf("== (j) Repeated components, offset / clip ==\n");
    {
        OrderPickingSolution s = repeatInstance();
        ConstraintAssembly assembly = copyOf("distanceFromPreviousPickTaskCost");
        assembly.add_instances(std::move(copyOf("distanceFromPreviousPickTaskCost")
                                            .weight("distanceFromPreviousPickTaskCost", 0.5)
                                            .exponent("distanceFromPreviousPickTaskCost", 2.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        checkScore("walk + 0.5 x walk^2 per trolley: -(10 + 25)", scoreWith(calculator, s), 0,
                   -35.0);
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].unit_costs == entries[1].unit_costs
                  && entries[0].unit_costs == std::vector<double>{5.0, 5.0, 0.0}
                  && entries[1].weight == 0.5 && entries[1].exponent == 2.0
                  && entries[1].default_weight == 1.0 && !entries[0].scoped,
              "two instances, the same raw costs, each with its own knobs, every trolley "
              "evaluated");
    }
    checkScore("threshold offset 3 on the walk: 2 + 2",
               repeatScore(copyOf("distanceFromPreviousPickTaskCost")
                               .offset("distanceFromPreviousPickTaskCost", 3.0)),
               0, -4.0);
    checkScore("literal offset 3 on the walk: 2 + 2 - 3, the empty trolley included",
               repeatScore(copyOf("distanceFromPreviousPickTaskCost")
                               .offset("distanceFromPreviousPickTaskCost", 3.0)
                               .clip("distanceFromPreviousPickTaskCost", false)),
               0, -1.0);
    checkScore("threshold offset -1 on the orders: 1000 x (3 + 2), the empty one 0",
               repeatScore(copyOf("orderSplitByTrolleyCost").offset("orderSplitByTrolleyCost", -1.0)),
               0, -5000.0);
    checkScore("literal offset -1 on the orders: 1000 x (3 + 2 + 1)",
               repeatScore(copyOf("orderSplitByTrolleyCost")
                               .offset("orderSplitByTrolleyCost", -1.0)
                               .clip("orderSplitByTrolleyCost", false)),
               0, -6000.0);
    // The literal bucket pick on soft: (0 - 1.5) + (1 - 1.5) + (0 - 1.5) =
    // -3.5, the empty trolley included: +3.5 soft (not rounded).
    checkScore("buckets literal offset 1.5 on soft: -1.5 - 0.5 - 1.5 -> +3.5 soft",
               repeatScore(copyOf("trolleyCapacityCost")
                               .soft("trolleyCapacityCost")
                               .offset("trolleyCapacityCost", 1.5)
                               .clip("trolleyCapacityCost", false)),
               0, 3.5);
    // A hard threshold pick rounds per trolley: the walk moved to hard at
    // weight 0.5 beyond 2 m: llround(1.5) x 2 = 4 (a rounded total would
    // give 3).
    checkScore("walk on hard, threshold offset 2 at weight 0.5: llround per trolley, -4 hard",
               repeatScore(copyOf("distanceFromPreviousPickTaskCost")
                               .hard("distanceFromPreviousPickTaskCost")
                               .weight("distanceFromPreviousPickTaskCost", 0.5)
                               .offset("distanceFromPreviousPickTaskCost", 2.0)),
               -4, 0.0);
    {
        // On hard the literal formula with an offset is refused at reset,
        // whichever sign and setter order: +1.5 would reward the trolleys
        // below it (+5 hard, so one trolley's spare buckets would cancel
        // another's shortage), -1.5 would charge three trolleys that hold no
        // violation.
        auto resetRefusal = [](ConstraintAssembly assembly) {
            try
            {
                repeatScore(std::move(assembly));
            }
            catch (const std::logic_error &error)
            {
                return std::string(error.what());
            }
            return std::string();
        };
        auto refusal = [](const std::string &name) {
            return "Constraint '" + name
                   + "' cannot use clip=false with a non-zero offset on a level that counts toward "
                     "the hard score (hard 0 must mean no violation)";
        };
        check(resetRefusal(copyOf("trolleyCapacityCost")
                               .offset("trolleyCapacityCost", 1.5)
                               .clip("trolleyCapacityCost", false))
                      == refusal("trolleyCapacityCost")
                  && resetRefusal(copyOf("trolleyCapacityCost")
                                      .clip("trolleyCapacityCost", false)
                                      .offset("trolleyCapacityCost", -1.5))
                         == refusal("trolleyCapacityCost"),
              "buckets literal offset +-1.5 on hard (its default): refused at reset");
        check(resetRefusal(copyOf("distanceFromPreviousPickTaskCost")
                               .offset("distanceFromPreviousPickTaskCost", 3.0)
                               .clip("distanceFromPreviousPickTaskCost", false)
                               .hard("distanceFromPreviousPickTaskCost"))
                  == refusal("distanceFromPreviousPickTaskCost"),
              "walk literal offset moved to hard afterwards: refused at reset");
        ConstraintAssembly repeated = copyOf("trolleyCapacityCost");
        repeated.add_instances(std::move(copyOf("trolleyCapacityCost")
                                            .offset("trolleyCapacityCost", 1.0)
                                            .clip("trolleyCapacityCost", false)));
        check(resetRefusal(std::move(repeated)) == refusal("trolleyCapacityCost"),
              "a repeated bucket instance with the literal offset on hard: refused at reset");
        check(resetRefusal(copyOf("trolleyCapacityCost").offset("trolleyCapacityCost", 1.5)).empty()
                  && resetRefusal(copyOf("trolleyCapacityCost").clip("trolleyCapacityCost", false))
                         .empty(),
              "allowed on hard: a threshold offset, or clip=false with offset 0");
    }
    checkScore("identical instances add up: 2 x 1000 x 3",
               repeatScore(std::move(copyOf("orderSplitByTrolleyCost")
                                         .add_instances(copyOf("orderSplitByTrolleyCost")))),
               0, -6000.0);
    using Descriptor = optimaru::SolutionTraits<OrderPickingSolution>;
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        OrderPickingSolution s = assignedDemo(4, 41);
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::Editor<OrderPickingSolution> editor(s, calculator);
        std::mt19937_64 rng(43);
        int mismatches = 0;
        double drift = 0.0;
        const int steps = 1500;
        for (int step = 0; step < steps; ++step)
        {
            const int list_count = Descriptor::list_count(s);
            const int from = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
            if (Descriptor::list_size(s, from) == 0)
                continue;
            const int position = static_cast<int>(
                rng() % static_cast<std::uint64_t>(Descriptor::list_size(s, from)));
            const int element = Descriptor::element_at(s, from, position);
            editor.remove(from, position);
            const int to = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
            const int insert_at = static_cast<int>(
                rng() % static_cast<std::uint64_t>(Descriptor::list_size(s, to) + 1));
            editor.insert(to, insert_at, element);

            const Score incremental = editor.score();
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            const Score full = scoreWith(fresh, s);
            drift = std::max(drift, std::fabs(full.soft - incremental.soft));
            if (!optimaru::scores_agree(incremental, full))
                ++mismatches;
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0,
              std::to_string(steps)
                  + " random list moves with repeated, shifted components: incremental == fresh");
    }
    {
        // Every preview == the applied score, re-checked under ScoreChecks::Full.
        OrderPickingSolution s = assignedDemo(4, 47);
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::Editor<OrderPickingSolution> editor(
            s, calculator, optimaru::ScoreChecks::Full);
        editor.score();
        optimaru::testing::PreviewParity parity;
        bool corrupted = false;
        try
        {
            parity = optimaru::testing::run_preview_parity(editor, calculator, /*seed=*/13,
                                                         /*trials=*/3000);
        }
        catch (const optimaru::ScoreMismatchError &error)
        {
            corrupted = true;
            std::printf("    %s\n", error.what());
        }
        std::printf("    %d moves checked, %d previewed, %d declined\n", parity.checked,
                    parity.previewed, parity.declined);
        check(!corrupted && parity.all_equal() && parity.declined == 0 && parity.previewed > 1000,
              "repeated, shifted components: every preview served and equal to the applied "
              "score (FULL_ASSERT)");
    }
    {
        OrderPickingSolution s = buildDemoData(3, 4, 60 * 40 * 20, 3, 37);
        Solver::Config config;
        config.time_limit = std::chrono::seconds(2);
        config.seed = 3;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruinRecreateUnimprovedStepTrigger = 200;
        config.ruinRecreatePolishSteps = 100;
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        bool corrupted = false;
        try
        {
            Solver solver(config);
            solver.solve(s, calculator);
        }
        catch (const optimaru::ScoreMismatchError &e)
        {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted, "2s FULL_ASSERT solve with repeated, shifted components");
        DecomposedCalculator fresh;
        repeatedAssembly().into(fresh);
        check(!corrupted && optimaru::scores_agree(s.score, scoreWith(fresh, s)),
              "the reported score == a fresh recompute of the same assembly");
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
    verifyAssemblyBuilder();
    verifyRawCostsByHand();
    verifyCatalog();
    verifyRepeatsAndOffsets();

    if (failures == 0)
    {
        std::printf("All order-picking plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d order-picking plugin check(s) FAILED.\n", failures);
    return 1;
}

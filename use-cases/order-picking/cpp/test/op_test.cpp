// Order-picking unit tests (engine test/ style: check() + non-zero exit on any
// failure):
//   (a) Warehouse.calculateDistance against every case of the Java
//       WarehouseTest (both directions)
//   (b) every constraint verified in isolation against the Java
//       OrderPickingConstraintProviderTest hand-computed cases
//   (c) shadow variables (trolley / previous / next) re-derived from lists
//   (d) incremental score == full recompute across a long random walk driven
//       through the engine's ScoreDirector protocol (before/mutate/after)
//   (e) demo-data reproduction invariants (catalog, family shelvings,
//       determinism)
//   (f) a demo instance solved under EnvironmentMode::FULL_ASSERT, plus a
//       plain solve that must reach feasibility
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "op/demo_data.hpp"
#include "op/domain.hpp"
#include "op/incremental_calculator.hpp"
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

WarehouseLocation loc(char column, int shelvingRow1Based, Side side, int row)
{
    WarehouseLocation location;
    location.column = column - 'A';
    location.shelvingRow = shelvingRow1Based - 1;
    location.side = side;
    location.row = row;
    return location;
}

// -------------------------------------------------------------------------
// (a) Warehouse distances: the Java WarehouseTest parameterized cases.
// -------------------------------------------------------------------------
void verifyWarehouseDistances()
{
    std::printf("== (a) Warehouse.calculateDistance (Java WarehouseTest) ==\n");
    struct Case
    {
        WarehouseLocation start;
        WarehouseLocation end;
        int expected;
    };
    const Side L = Side::LEFT;
    const Side R = Side::RIGHT;
    const std::vector<Case> cases = {
        // same shelving
        {loc('B', 3, L, 1), loc('B', 3, L, 1), 0},
        {loc('B', 3, L, 1), loc('B', 3, R, 1), 4},
        {loc('B', 3, L, 5), loc('B', 3, L, 5), 0},
        {loc('B', 3, L, 1), loc('B', 3, L, 10), 9},
        {loc('B', 3, L, 1), loc('B', 3, L, 5), 4},
        {loc('B', 3, R, 10), loc('B', 3, R, 10), 0},
        {loc('B', 3, R, 2), loc('B', 3, R, 2), 0},
        {loc('B', 3, R, 2), loc('B', 3, R, 8), 6},
        {loc('B', 3, R, 10), loc('B', 3, R, 1), 9},
        {loc('B', 3, R, 9), loc('B', 3, R, 1), 8},
        {loc('B', 3, L, 2), loc('B', 3, R, 4), 8},
        {loc('B', 3, L, 8), loc('B', 3, R, 7), 7},
        // neighbor shelvings on the same warehouse row
        {loc('C', 2, L, 1), loc('D', 2, R, 3), 11},
        {loc('C', 2, L, 1), loc('D', 2, L, 3), 9},
        {loc('C', 2, R, 1), loc('D', 2, L, 3), 5},
        {loc('C', 2, R, 1), loc('D', 2, R, 3), 9},
        {loc('C', 2, L, 8), loc('D', 2, R, 9), 10},
        {loc('C', 2, L, 8), loc('D', 2, L, 9), 8},
        {loc('C', 2, R, 8), loc('D', 2, L, 9), 4},
        {loc('C', 2, R, 8), loc('D', 2, R, 9), 8},
        // non-neighbor shelvings on the same warehouse row
        {loc('C', 3, L, 1), loc('E', 3, R, 3), 16},
        {loc('C', 3, L, 1), loc('E', 3, L, 3), 14},
        {loc('C', 3, R, 1), loc('E', 3, L, 3), 12},
        {loc('C', 3, R, 1), loc('E', 3, R, 3), 14},
        {loc('C', 3, L, 9), loc('E', 3, R, 8), 15},
        {loc('C', 3, L, 9), loc('E', 3, L, 8), 13},
        {loc('C', 3, R, 9), loc('E', 3, L, 8), 11},
        {loc('C', 3, R, 9), loc('E', 3, R, 8), 13},
        // shelvings on different warehouse rows
        {loc('C', 1, L, 1), loc('E', 3, R, 3), 40},
        {loc('C', 1, L, 1), loc('E', 3, L, 3), 38},
        {loc('C', 1, R, 1), loc('E', 3, L, 3), 36},
        {loc('C', 1, R, 1), loc('E', 3, R, 3), 38},
        {loc('B', 2, L, 9), loc('B', 3, R, 8), 14},
        {loc('B', 2, L, 9), loc('B', 3, L, 8), 12},
        {loc('B', 2, R, 9), loc('B', 3, L, 8), 14},
        {loc('B', 2, R, 12), loc('B', 3, R, 11), 12},
    };
    bool allOk = true;
    for (const Case &c : cases)
    {
        const int forward = calculateDistance(c.start, c.end);
        const int backward = calculateDistance(c.end, c.start);
        if (forward != c.expected || backward != c.expected)
        {
            allOk = false;
            std::printf("    distance %s(%d,%d) <-> %s(%d,%d): got %d/%d, "
                        "expected %d\n",
                        c.start.shelvingId().c_str(),
                        static_cast<int>(c.start.side),
                        c.start.row,
                        c.end.shelvingId().c_str(),
                        static_cast<int>(c.end.side),
                        c.end.row,
                        forward,
                        backward,
                        c.expected);
        }
    }
    check(allOk,
          "all " + std::to_string(cases.size())
              + " WarehouseTest distances match (both directions)");
}

// -------------------------------------------------------------------------
// (b) Constraints in isolation (Java OrderPickingConstraintProviderTest).
// -------------------------------------------------------------------------

// Appends a pick task of `order` with `volume` at `location`; returns its
// index.
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

void verifyConstraints()
{
    std::printf("== (b) Verifying each constraint in isolation ==\n");
    const WarehouseLocation origin = loc('A', 1, Side::LEFT, 0);

    // requiredNumberOfBuckets, the Java test's exact data:
    // order1 volumes 4,5 / 9,8; order2 volumes 4,1 / 8,6,10,9.
    // Trolley1 (2 buckets, capacity 5): order1 9 -> 2, order2 5 -> 1; 3-2=1.
    // Trolley2 (2 buckets, capacity 10): order1 17 -> 2, order2 33 -> 4; 6-2=4.
    {
        OrderPickingSolution s;
        s.orders = {{"order1"}, {"order2"}};
        const int t1 = addTrolley(s, 2, 5, origin);
        const int t2 = addTrolley(s, 2, 10, origin);
        for (int volume : {4, 5})
            s.trolleys[t1].pickTaskIndices.push_back(
                addPick(s, 0, volume, origin));
        for (int volume : {4, 1})
            s.trolleys[t1].pickTaskIndices.push_back(
                addPick(s, 1, volume, origin));
        for (int volume : {9, 8})
            s.trolleys[t2].pickTaskIndices.push_back(
                addPick(s, 0, volume, origin));
        for (int volume : {8, 6, 10, 9})
            s.trolleys[t2].pickTaskIndices.push_back(
                addPick(s, 1, volume, origin));
        const ConstraintTotals totals = ScoreCalculator::calculateTotals(s);
        check(totals.requiredNumberOfBuckets == 5,
              "requiredNumberOfBuckets penalizes by 5 (got "
                  + std::to_string(totals.requiredNumberOfBuckets) + ")");
        check(totals.toScore().hard == -5,
              "bucket shortage lands on the hard level");
    }

    // minimizeDistanceFromPreviousPickTask: previous at (E,1) RIGHT 3,
    // current at (C,3) RIGHT 1 -> 34 (the Java case). The trolley parks at
    // the previous pick's location so the first leg is 0.
    {
        OrderPickingSolution s;
        s.orders = {{"order1"}};
        const WarehouseLocation previous = loc('E', 1, Side::RIGHT, 3);
        const WarehouseLocation current = loc('C', 3, Side::RIGHT, 1);
        const int t = addTrolley(s, 1, 100, previous);
        s.trolleys[t].pickTaskIndices.push_back(addPick(s, 0, 1, previous));
        s.trolleys[t].pickTaskIndices.push_back(addPick(s, 0, 1, current));
        const ConstraintTotals totals = ScoreCalculator::calculateTotals(s);
        check(totals.distanceFromPreviousPickTask == 34,
              "minimizeDistanceFromPreviousPickTask penalizes by 34 (got "
                  + std::to_string(totals.distanceFromPreviousPickTask) + ")");
    }

    // minimizeDistanceFromLastPickTaskToPathOrigin: last pick at (D,2) LEFT
    // 0, path origin (A,1) LEFT 0 -> 28 (the Java case). All three picks sit
    // on the last pick's spot, so only the return leg is 28.
    {
        OrderPickingSolution s;
        s.orders = {{"order1"}};
        const WarehouseLocation last = loc('D', 2, Side::LEFT, 0);
        const int t = addTrolley(s, 1, 100, origin);
        for (int i = 0; i < 3; ++i)
            s.trolleys[t].pickTaskIndices.push_back(addPick(s, 0, 1, last));
        const ConstraintTotals totals = ScoreCalculator::calculateTotals(s);
        check(totals.distanceFromLastPickTaskToPathOrigin == 28,
              "minimizeDistanceFromLastPickTaskToPathOrigin penalizes by 28 "
              "(got "
                  + std::to_string(totals.distanceFromLastPickTaskToPathOrigin)
                  + ")");
        // Sanity on the whole route: origin->last 28, two zero-length legs,
        // return 28.
        check(totals.distanceFromPreviousPickTask == 28,
              "previous-pick legs sum to 28 (origin -> first)");
    }

    // minimizeOrderSplitByTrolley, the Java test's exact data: order1 spread
    // over 3 trolleys, order2 on 1 trolley -> (3 + 1) * 1000.
    {
        OrderPickingSolution s;
        s.orders = {{"order1"}, {"order2"}};
        const int t1 = addTrolley(s, 2, 1, origin);
        const int t2 = addTrolley(s, 1, 1, origin);
        const int t3 = addTrolley(s, 1, 1, origin);
        const int t4 = addTrolley(s, 4, 1, origin);
        for (int i = 0; i < 2; ++i)
            s.trolleys[t1].pickTaskIndices.push_back(addPick(s, 0, 1, origin));
        s.trolleys[t2].pickTaskIndices.push_back(addPick(s, 0, 1, origin));
        s.trolleys[t3].pickTaskIndices.push_back(addPick(s, 0, 1, origin));
        for (int i = 0; i < 4; ++i)
            s.trolleys[t4].pickTaskIndices.push_back(addPick(s, 1, 1, origin));
        const ConstraintTotals totals = ScoreCalculator::calculateTotals(s);
        check(totals.orderSplitByTrolley == 4000,
              "minimizeOrderSplitByTrolley penalizes by 4 * 1000 (got "
                  + std::to_string(totals.orderSplitByTrolley) + ")");
    }

    // Unassigned pick tasks contribute nothing (forEach skips them).
    {
        OrderPickingSolution s;
        s.orders = {{"order1"}};
        addTrolley(s, 1, 1, origin);
        addPick(s, 0, 1000, loc('E', 3, Side::RIGHT, 9));  // in no list
        const Score score = ScoreCalculator::calculateScore(s);
        check(score.hard == 0 && score.soft == 0.0,
              "unassigned pick task contributes nothing");
    }
}

// -------------------------------------------------------------------------
// (c) Shadow variables re-derived from the lists.
// -------------------------------------------------------------------------
void verifyShadowVariables()
{
    std::printf("== (c) Shadow variables (trolley / previous / next) ==\n");
    const WarehouseLocation origin = loc('A', 1, Side::LEFT, 0);
    OrderPickingSolution s;
    s.orders = {{"order1"}};
    const int t1 = addTrolley(s, 1, 100, origin);
    const int t2 = addTrolley(s, 1, 100, origin);
    const int a = addPick(s, 0, 1, origin);
    const int b = addPick(s, 0, 1, origin);
    const int c = addPick(s, 0, 1, origin);
    const int d = addPick(s, 0, 1, origin);  // stays unassigned
    s.trolleys[t1].pickTaskIndices = {a, b, c};
    ScoreCalculator::updateShadowVariables(s);
    check(s.pickTasks[a].trolleyIndex == t1 && s.pickTasks[b].trolleyIndex == t1
              && s.pickTasks[c].trolleyIndex == t1,
          "inverse-relation shadow: trolley index set for assigned picks");
    check(s.pickTasks[a].previousPickTask == -1
              && s.pickTasks[b].previousPickTask == a
              && s.pickTasks[c].previousPickTask == b,
          "previous-element shadow follows the list order");
    check(s.pickTasks[a].nextPickTask == b && s.pickTasks[b].nextPickTask == c
              && s.pickTasks[c].nextPickTask == -1,
          "next-element shadow follows the list order (last has null)");
    check(s.pickTasks[c].isLast() && !s.pickTasks[b].isLast(),
          "isLast() == (nextPickTask == null)");
    check(s.pickTasks[d].trolleyIndex == -1
              && s.pickTasks[d].previousPickTask == -1
              && s.pickTasks[d].nextPickTask == -1,
          "unassigned pick keeps null shadows");
    // Moving the chain to the other trolley re-derives everything.
    s.trolleys[t1].pickTaskIndices.clear();
    s.trolleys[t2].pickTaskIndices = {c, a};
    ScoreCalculator::updateShadowVariables(s);
    check(s.pickTasks[c].trolleyIndex == t2 && s.pickTasks[a].trolleyIndex == t2
              && s.pickTasks[a].previousPickTask == c
              && s.pickTasks[b].trolleyIndex == -1,
          "shadows re-derived after the lists changed");
}

// -------------------------------------------------------------------------
// (d) Incremental score == full recompute across a random walk of list
// mutations, driven through the ScoreDirector protocol.
// -------------------------------------------------------------------------
void verifyIncrementalAgainstFull()
{
    std::printf("== (d) Incremental == full recompute (random walk) ==\n");
    OrderPickingSolution s = buildDemoData();
    const int taskCount = static_cast<int>(s.pickTasks.size());
    const int trolleyCount = static_cast<int>(s.trolleys.size());

    OrderPickingIncrementalScoreCalculator calculator;
    optimaru::ScoreDirector<OrderPickingSolution> director(s, calculator);

    std::mt19937_64 rng(42);
    auto randomInt = [&rng](int bound)
    { return static_cast<int>(rng() % static_cast<std::uint64_t>(bound)); };

    // position[e] = (trolley, position) or (-1, -1) when unassigned.
    auto findElement = [&s](int element) -> std::pair<int, int>
    {
        for (int t = 0; t < static_cast<int>(s.trolleys.size()); ++t)
        {
            const auto &list = s.trolleys[t].pickTaskIndices;
            for (int p = 0; p < static_cast<int>(list.size()); ++p)
                if (list[p] == element)
                    return {t, p};
        }
        return {-1, -1};
    };

    int mismatches = 0;
    const int iterations = 1500;
    for (int i = 0; i < iterations; ++i)
    {
        const int op = randomInt(100);
        if (op < 70)
        {
            // Relocate (or first-assign) a random element.
            const int element = randomInt(taskCount);
            const auto [t, p] = findElement(element);
            if (t != -1)
                director.removeListElement(t, p);
            const int target = randomInt(trolleyCount);
            const int position = randomInt(
                static_cast<int>(s.trolleys[target].pickTaskIndices.size())
                + 1);
            director.insertListElement(target, position, element);
        }
        else if (op < 85)
        {
            // Reverse a random segment.
            const int t = randomInt(trolleyCount);
            const int size
                = static_cast<int>(s.trolleys[t].pickTaskIndices.size());
            if (size < 2)
                continue;
            int first = randomInt(size);
            int last = randomInt(size);
            if (first > last)
                std::swap(first, last);
            if (first == last)
                continue;
            director.reverseListSegment(t, first, last);
        }
        else
        {
            // Unassign a random assigned element (partial solutions must
            // score correctly too — the construction heuristic relies on it).
            const int element = randomInt(taskCount);
            const auto [t, p] = findElement(element);
            if (t == -1)
                continue;
            director.removeListElement(t, p);
        }

        const Score incremental = director.calculateScore();
        OrderPickingSolution copy = s;
        const Score full = ScoreCalculator::calculateScore(copy);
        if (incremental.hard != full.hard
            || std::fabs(incremental.soft - full.soft) > 1e-9)
        {
            if (++mismatches <= 3)
            {
                std::printf("    mismatch after op %d: incremental=%s "
                            "full=%s\n",
                            i,
                            incremental.toString().c_str(),
                            full.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(iterations)
              + " random list mutations: incremental == full recompute");
}

// -------------------------------------------------------------------------
// (e) Demo-data reproduction invariants.
// -------------------------------------------------------------------------
void verifyDemoData()
{
    std::printf("== (e) Demo data (DemoDataGenerator reproduction) ==\n");
    OrderPickingSolution s = buildDemoData();
    check(s.trolleys.size() == 5, "5 trolleys");
    check(s.orders.size() == 8, "8 orders");
    check(demoMaxProductSize() == 50 * 20 * 20,
          "max product volume is the toilet tissue (20000 cm3)");
    bool valid = !s.pickTasks.empty();
    for (const PickTask &task : s.pickTasks)
    {
        valid = valid && task.volume > 0 && task.volume <= demoMaxProductSize()
                && task.orderIndex >= 0
                && task.orderIndex < static_cast<int>(s.orders.size())
                && task.location.column >= 0
                && task.location.column < Warehouse::COLUMN_COUNT
                && task.location.shelvingRow >= 0
                && task.location.shelvingRow < Warehouse::ROW_COUNT
                && task.location.row >= 1
                && task.location.row <= Warehouse::SHELVING_ROWS_SIZE;
    }
    check(valid,
          "every pick task has a demo product volume and a valid location ("
              + std::to_string(s.pickTasks.size()) + " picks)");
    for (const Trolley &trolley : s.trolleys)
    {
        valid = valid && trolley.bucketCount == 4
                && trolley.bucketCapacity == 60 * 40 * 20
                && trolley.location.shelvingId() == "(A,1)"
                && trolley.location.side == Side::LEFT
                && trolley.location.row == 0;
    }
    check(valid, "trolleys: 4 buckets x 48000 cm3, parked at (A,1) LEFT 0");

    OrderPickingSolution again = buildDemoData();
    bool same = again.pickTasks.size() == s.pickTasks.size();
    for (std::size_t i = 0; same && i < s.pickTasks.size(); ++i)
    {
        same
            = again.pickTasks[i].id == s.pickTasks[i].id
              && again.pickTasks[i].volume == s.pickTasks[i].volume
              && again.pickTasks[i].location.shelvingId()
                     == s.pickTasks[i].location.shelvingId()
              && again.pickTasks[i].location.side
                     == s.pickTasks[i].location.side
              && again.pickTasks[i].location.row == s.pickTasks[i].location.row;
    }
    check(same, "generation is deterministic (java.util.Random(37) replay)");

    bool throws = false;
    try
    {
        buildDemoData(5, 4, /*bucketCapacity too small*/ 100, 8, 37);
    }
    catch (const std::invalid_argument &)
    {
        throws = true;
    }
    check(throws, "bucketCapacity below the max product size is rejected");
}

// -------------------------------------------------------------------------
// (f) Solving: FULL_ASSERT consistency + feasibility on the demo instance.
// -------------------------------------------------------------------------
void verifySolve()
{
    std::printf("== (f) Solve: FULL_ASSERT + feasibility ==\n");

    // FULL_ASSERT: every move's incremental score re-verified against a full
    // recompute; ScoreCorruptionError fails the test. Small instance + short
    // LNS cycles keep the quadratic assert cost inside the 2s budget while
    // still exercising the ruin/recreate paths.
    {
        OrderPickingSolution s = buildDemoData(3, 4, 60 * 40 * 20, 3, 37);
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(2);
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        config.ruinRecreateUnimprovedStepTrigger = 100;
        config.ruinRecreatePolishSteps = 50;
        config.randomSeed = 1;
        bool threw = false;
        try
        {
            Solver(config).solve(s);
        }
        catch (const optimaru::ScoreCorruptionError &)
        {
            threw = true;
        }
        check(!threw, "2s solve under FULL_ASSERT (incremental never drifts)");
    }

    // Plain solve on the full demo instance: must assign every pick exactly
    // once and reach hard 0 (the Java demo dataset is comfortably feasible).
    {
        OrderPickingSolution s = buildDemoData();
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(4);
        config.bestScoreLimit = {0, -1e18};  // stop once feasible
        config.randomSeed = 7;
        Solver(config).solve(s);

        std::vector<int> seen(s.pickTasks.size(), 0);
        for (const Trolley &trolley : s.trolleys)
            for (int pickIndex : trolley.pickTaskIndices)
                ++seen[pickIndex];
        bool conserved = true;
        for (int count : seen)
            conserved = conserved && count == 1;
        check(conserved, "every pick task assigned exactly once");
        check(s.score.isFeasible(),
              "feasible (hard 0) reached, score " + s.score.toString());

        OrderPickingSolution copy = s;
        const Score recomputed = ScoreCalculator::calculateScore(copy);
        check(s.score.hard == recomputed.hard
                  && std::fabs(s.score.soft - recomputed.soft) < 1e-9,
              "returned score matches a from-scratch recompute");

        // The final shadows describe the final routes.
        bool shadowsOk = true;
        for (int t = 0; t < static_cast<int>(s.trolleys.size()); ++t)
        {
            const auto &list = s.trolleys[t].pickTaskIndices;
            for (int p = 0; p < static_cast<int>(list.size()); ++p)
            {
                const PickTask &task = s.pickTasks[list[p]];
                shadowsOk
                    = shadowsOk && task.trolleyIndex == t
                      && task.previousPickTask == (p > 0 ? list[p - 1] : -1)
                      && task.nextPickTask
                             == (p + 1 < static_cast<int>(list.size())
                                     ? list[p + 1]
                                     : -1);
            }
        }
        check(shadowsOk, "shadow variables consistent on the final solution");
    }
}

}  // namespace

int main()
{
    verifyWarehouseDistances();
    verifyConstraints();
    verifyShadowVariables();
    verifyIncrementalAgainstFull();
    verifyDemoData();
    verifySolve();

    if (failures > 0)
    {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}

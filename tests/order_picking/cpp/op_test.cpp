// Order-picking unit tests (engine test/ style: check() + non-zero exit on any
// failure):
//   (a) calculateDistance against hand-computed cases (both directions)
//   (b) every constraint verified in isolation against hand-computed cases
//   (c) derived fields (trolley / previous / next) re-derived from lists
//   (d) incremental score == full recompute across a long random walk driven
//       through the engine's Editor protocol (before/mutate/after)
//   (e) demo-data reproduction invariants (catalog, family shelvings,
//       determinism)
//   (f) a demo instance solved under ScoreChecks::ScoreChecks::Full, plus a
//       plain solve that must reach feasibility
//   (g) other layouts: a hand-computed walk on a grid of another size and on
//       named locations with a distance table, the checks of both, a warm
//       start, solves under ScoreChecks::Full, the solve's distance table
//       (the grid arithmetic's numbers, gone afterwards, skipped above its
//       size limit)
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "op/model/demo_data.hpp"
#include "op/model/domain.hpp"
#include "op/model/layout.hpp"
#include "op/score/incremental_calculator.hpp"
#include "op/score/score_calculator.hpp"
#include "op/solver.hpp"
#include "optimaru/solution/editor.hpp"
#include "optimaru/testing/check.hpp"

using namespace op;

namespace {

using optimaru::testing::check;
using optimaru::testing::failures;

WarehouseLocation loc(char column, int shelvingRow1Based, Side side, int row) {
    WarehouseLocation location;
    location.column = column - 'A';
    location.shelvingRow = shelvingRow1Based - 1;
    location.side = side;
    location.row = row;
    return location;
}

// -------------------------------------------------------------------------
// (a) Warehouse distances: hand-computed cases.
// -------------------------------------------------------------------------
void verifyWarehouseDistances() {
    std::printf("== (a) Warehouse calculateDistance ==\n");
    struct Case {
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
    for (const Case& c : cases) {
        const int forward = calculateDistance(c.start, c.end);
        const int backward = calculateDistance(c.end, c.start);
        if (forward != c.expected || backward != c.expected) {
            allOk = false;
            std::printf(
                    "    distance %s(%d,%d) <-> %s(%d,%d): got %d/%d, "
                    "expected %d\n",
                    c.start.shelvingId().c_str(), static_cast<int>(c.start.side), c.start.row,
                    c.end.shelvingId().c_str(), static_cast<int>(c.end.side), c.end.row, forward,
                    backward, c.expected);
        }
    }
    check(allOk,
          "all " + std::to_string(cases.size()) + " warehouse distances match (both directions)");
}

// -------------------------------------------------------------------------
// (b) Constraints in isolation.
// -------------------------------------------------------------------------

// Appends a pick task of `order` with `volume` at `location`; returns its
// index.
int addPick(OrderPickingSolution& s, int orderIndex, int volume,
            const WarehouseLocation& location) {
    PickTask task;
    task.id = "p" + std::to_string(s.pickTasks.size());
    task.orderIndex = orderIndex;
    task.volume = volume;
    task.location = location;
    s.pickTasks.push_back(task);
    return static_cast<int>(s.pickTasks.size()) - 1;
}

int addTrolley(OrderPickingSolution& s, int bucketCount, int bucketCapacity,
               const WarehouseLocation& location) {
    Trolley trolley;
    trolley.id = "t" + std::to_string(s.trolleys.size());
    trolley.bucketCount = bucketCount;
    trolley.bucketCapacity = bucketCapacity;
    trolley.location = location;
    s.trolleys.push_back(trolley);
    return static_cast<int>(s.trolleys.size()) - 1;
}

void verifyConstraints() {
    std::printf("== (b) Verifying each constraint in isolation ==\n");
    const WarehouseLocation origin = loc('A', 1, Side::LEFT, 0);

    // trolleyCapacityCost:
    // order1 volumes 4,5 / 9,8; order2 volumes 4,1 / 8,6,10,9.
    // Trolley1 (2 buckets, capacity 5): order1 9 -> 2, order2 5 -> 1; 3-2=1.
    // Trolley2 (2 buckets, capacity 10): order1 17 -> 2, order2 33 -> 4; 6-2=4.
    {
        OrderPickingSolution s;
        s.orders = {{"order1"}, {"order2"}};
        const int t1 = addTrolley(s, 2, 5, origin);
        const int t2 = addTrolley(s, 2, 10, origin);
        for (int volume : {4, 5})
            s.trolleys[t1].pickTaskIndices.push_back(addPick(s, 0, volume, origin));
        for (int volume : {4, 1})
            s.trolleys[t1].pickTaskIndices.push_back(addPick(s, 1, volume, origin));
        for (int volume : {9, 8})
            s.trolleys[t2].pickTaskIndices.push_back(addPick(s, 0, volume, origin));
        for (int volume : {8, 6, 10, 9})
            s.trolleys[t2].pickTaskIndices.push_back(addPick(s, 1, volume, origin));
        const ConstraintTotals totals = ScoreCalculator::calculateTotals(s);
        check(totals.trolleyCapacityCost == 5, "trolleyCapacityCost penalizes by 5 (got " +
                                                       std::to_string(totals.trolleyCapacityCost) +
                                                       ")");
        check(totals.toScore().hard == -5, "bucket shortage lands on the hard level");
    }

    // distanceFromPreviousPickTaskCost: previous at (E,1) RIGHT 3,
    // current at (C,3) RIGHT 1 -> 34. The trolley parks at
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
        check(totals.distanceFromPreviousPickTaskCost == 34,
              "distanceFromPreviousPickTaskCost penalizes by 34 (got " +
                      std::to_string(totals.distanceFromPreviousPickTaskCost) + ")");
    }

    // distanceFromLastPickTaskToPathOriginCost: last pick at (D,2) LEFT
    // 0, path origin (A,1) LEFT 0 -> 28. All three picks sit
    // on the last pick's spot, so only the return leg is 28.
    {
        OrderPickingSolution s;
        s.orders = {{"order1"}};
        const WarehouseLocation last = loc('D', 2, Side::LEFT, 0);
        const int t = addTrolley(s, 1, 100, origin);
        for (int i = 0; i < 3; ++i) s.trolleys[t].pickTaskIndices.push_back(addPick(s, 0, 1, last));
        const ConstraintTotals totals = ScoreCalculator::calculateTotals(s);
        check(totals.distanceFromLastPickTaskToPathOriginCost == 28,
              "distanceFromLastPickTaskToPathOriginCost penalizes by 28 "
              "(got " +
                      std::to_string(totals.distanceFromLastPickTaskToPathOriginCost) + ")");
        // Sanity on the whole route: origin->last 28, two zero-length legs,
        // return 28.
        check(totals.distanceFromPreviousPickTaskCost == 28,
              "previous-pick legs sum to 28 (origin -> first)");
    }

    // orderSplitByTrolleyCost: order1 spread
    // over 3 trolleys, order2 on 1 trolley -> (3 + 1) * 1000 (the totals
    // carry the default weight 1000).
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
        check(totals.orderSplitByTrolleyCost == 4000,
              "orderSplitByTrolleyCost penalizes by 4 * 1000 (got " +
                      std::to_string(totals.orderSplitByTrolleyCost) + ")");
    }

    // Unassigned pick tasks contribute nothing.
    {
        OrderPickingSolution s;
        s.orders = {{"order1"}};
        addTrolley(s, 1, 1, origin);
        addPick(s, 0, 1000, loc('E', 3, Side::RIGHT, 9));  // in no list
        const Score score = ScoreCalculator::score(s);
        check(score.hard == 0 && score.soft == 0.0, "unassigned pick task contributes nothing");
    }
}

// -------------------------------------------------------------------------
// (c) Derived fields re-derived from the lists.
// -------------------------------------------------------------------------
void verifyShadowVariables() {
    std::printf("== (c) Derived fields (trolley / previous / next) ==\n");
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
    check(s.pickTasks[a].trolleyIndex == t1 && s.pickTasks[b].trolleyIndex == t1 &&
                  s.pickTasks[c].trolleyIndex == t1,
          "trolley index set for assigned picks");
    check(s.pickTasks[a].previousPickTask == -1 && s.pickTasks[b].previousPickTask == a &&
                  s.pickTasks[c].previousPickTask == b,
          "previous pick follows the list order");
    check(s.pickTasks[a].nextPickTask == b && s.pickTasks[b].nextPickTask == c &&
                  s.pickTasks[c].nextPickTask == -1,
          "next pick follows the list order (last has none)");
    check(s.pickTasks[c].isLast() && !s.pickTasks[b].isLast(), "isLast() == (nextPickTask == -1)");
    check(s.pickTasks[d].trolleyIndex == -1 && s.pickTasks[d].previousPickTask == -1 &&
                  s.pickTasks[d].nextPickTask == -1,
          "unassigned pick keeps -1 in every derived field");
    // Moving the chain to the other trolley re-derives everything.
    s.trolleys[t1].pickTaskIndices.clear();
    s.trolleys[t2].pickTaskIndices = {c, a};
    ScoreCalculator::updateShadowVariables(s);
    check(s.pickTasks[c].trolleyIndex == t2 && s.pickTasks[a].trolleyIndex == t2 &&
                  s.pickTasks[a].previousPickTask == c && s.pickTasks[b].trolleyIndex == -1,
          "derived fields re-derived after the lists changed");
}

// -------------------------------------------------------------------------
// (d) Incremental score == full recompute across a random walk of list
// mutations, driven through the Editor protocol.
// -------------------------------------------------------------------------
void verifyIncrementalAgainstFull() {
    std::printf("== (d) Incremental == full recompute (random walk) ==\n");
    OrderPickingSolution s = buildDemoData();
    const int taskCount = static_cast<int>(s.pickTasks.size());
    const int trolleyCount = static_cast<int>(s.trolleys.size());

    OrderPickingIncrementalScoreCalculator calculator;
    optimaru::Editor<OrderPickingSolution> editor(s, calculator);

    std::mt19937_64 rng(42);
    auto randomInt = [&rng](int bound) {
        return static_cast<int>(rng() % static_cast<std::uint64_t>(bound));
    };

    // position[e] = (trolley, position) or (-1, -1) when unassigned.
    auto findElement = [&s](int element) -> std::pair<int, int> {
        for (int t = 0; t < static_cast<int>(s.trolleys.size()); ++t) {
            const auto& list = s.trolleys[t].pickTaskIndices;
            for (int p = 0; p < static_cast<int>(list.size()); ++p)
                if (list[p] == element) return {t, p};
        }
        return {-1, -1};
    };

    int mismatches = 0;
    const int iterations = 1500;
    for (int i = 0; i < iterations; ++i) {
        const int op = randomInt(100);
        if (op < 70) {
            // Relocate (or first-assign) a random element.
            const int element = randomInt(taskCount);
            const auto [t, p] = findElement(element);
            if (t != -1) editor.remove(t, p);
            const int target = randomInt(trolleyCount);
            const int position =
                    randomInt(static_cast<int>(s.trolleys[target].pickTaskIndices.size()) + 1);
            editor.insert(target, position, element);
        } else if (op < 85) {
            // Reverse a random segment.
            const int t = randomInt(trolleyCount);
            const int size = static_cast<int>(s.trolleys[t].pickTaskIndices.size());
            if (size < 2) continue;
            int first = randomInt(size);
            int last = randomInt(size);
            if (first > last) std::swap(first, last);
            if (first == last) continue;
            editor.reverse(t, first, last);
        } else {
            // Unassign a random assigned element (partial solutions must
            // score correctly too — the construction heuristic relies on it).
            const int element = randomInt(taskCount);
            const auto [t, p] = findElement(element);
            if (t == -1) continue;
            editor.remove(t, p);
        }

        const Score incremental = editor.score();
        OrderPickingSolution copy = s;
        const Score full = ScoreCalculator::score(copy);
        if (incremental.hard != full.hard || std::fabs(incremental.soft - full.soft) > 1e-9) {
            if (++mismatches <= 3) {
                std::printf(
                        "    mismatch after op %d: incremental=%s "
                        "full=%s\n",
                        i, incremental.str().c_str(), full.str().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(iterations) + " random list mutations: incremental == full recompute");
}

// -------------------------------------------------------------------------
// (e) Demo-data reproduction invariants.
// -------------------------------------------------------------------------
void verifyDemoData() {
    std::printf("== (e) Demo data ==\n");
    OrderPickingSolution s = buildDemoData();
    check(s.trolleys.size() == 5, "5 trolleys");
    check(s.orders.size() == 8, "8 orders");
    check(demoMaxProductSize() == 50 * 20 * 20,
          "max product volume is the toilet tissue (20000 cm3)");
    bool valid = !s.pickTasks.empty();
    for (const PickTask& task : s.pickTasks) {
        valid = valid && task.volume > 0 && task.volume <= demoMaxProductSize() &&
                task.orderIndex >= 0 && task.orderIndex < static_cast<int>(s.orders.size()) &&
                task.location.column >= 0 &&
                task.location.column < Warehouse::DEFAULT_COLUMN_COUNT &&
                task.location.shelvingRow >= 0 &&
                task.location.shelvingRow < Warehouse::DEFAULT_SHELVING_ROW_COUNT &&
                task.location.row >= 1 && task.location.row <= Warehouse::DEFAULT_SHELVING_HEIGHT;
    }
    check(valid, "every pick task has a demo product volume and a valid location (" +
                         std::to_string(s.pickTasks.size()) + " picks)");
    for (const Trolley& trolley : s.trolleys) {
        valid = valid && trolley.bucketCount == 4 && trolley.bucketCapacity == 60 * 40 * 20 &&
                trolley.location.shelvingId() == "(A,1)" && trolley.location.side == Side::LEFT &&
                trolley.location.row == 0;
    }
    check(valid, "trolleys: 4 buckets x 48000 cm3, parked at (A,1) LEFT 0");

    OrderPickingSolution again = buildDemoData();
    bool same = again.pickTasks.size() == s.pickTasks.size();
    for (std::size_t i = 0; same && i < s.pickTasks.size(); ++i) {
        same = again.pickTasks[i].id == s.pickTasks[i].id &&
               again.pickTasks[i].volume == s.pickTasks[i].volume &&
               again.pickTasks[i].location.shelvingId() == s.pickTasks[i].location.shelvingId() &&
               again.pickTasks[i].location.side == s.pickTasks[i].location.side &&
               again.pickTasks[i].location.row == s.pickTasks[i].location.row;
    }
    check(same, "generation is deterministic (seed 37)");

    bool throws = false;
    try {
        buildDemoData(5, 4, /*bucketCapacity too small*/ 100, 8, 37);
    } catch (const std::invalid_argument&) {
        throws = true;
    }
    check(throws, "bucketCapacity below the max product size is rejected");
}

// -------------------------------------------------------------------------
// (f) Solving: ScoreChecks::Full consistency + feasibility on the demo instance.
// -------------------------------------------------------------------------
void verifySolve() {
    std::printf("== (f) Solve: FULL_ASSERT + feasibility ==\n");

    // ScoreChecks::Full: every move's incremental score re-verified against a full
    // recompute; ScoreMismatchError fails the test. Small instance + short
    // LNS cycles keep the quadratic assert cost inside the 2s budget while
    // still exercising the ruin/recreate paths.
    {
        OrderPickingSolution s = buildDemoData(3, 4, 60 * 40 * 20, 3, 37);
        Solver::Config config;
        config.time_limit = std::chrono::seconds(2);
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruin_recreate_unimproved_step_trigger = 100;
        config.ruin_recreate_polish_steps = 50;
        config.seed = 1;
        bool threw = false;
        try {
            Solver(config).solve(s);
        } catch (const optimaru::ScoreMismatchError&) {
            threw = true;
        }
        check(!threw, "2s solve under FULL_ASSERT (incremental never drifts)");
    }

    // Plain solve on the full demo instance: must assign every pick exactly
    // once and reach hard 0 (the demo warehouse is comfortably feasible).
    {
        OrderPickingSolution s = buildDemoData();
        Solver::Config config;
        config.time_limit = std::chrono::seconds(4);
        config.target_score = {0, -std::numeric_limits<double>::infinity()};  // stop once feasible
        config.seed = 7;
        Solver(config).solve(s);

        std::vector<int> seen(s.pickTasks.size(), 0);
        for (const Trolley& trolley : s.trolleys)
            for (int pickIndex : trolley.pickTaskIndices) ++seen[pickIndex];
        bool conserved = true;
        for (int count : seen) conserved = conserved && count == 1;
        check(conserved, "every pick task assigned exactly once");
        check(s.score.feasible(), "feasible (hard 0) reached, score " + s.score.str());

        OrderPickingSolution copy = s;
        const Score recomputed = ScoreCalculator::score(copy);
        check(s.score.hard == recomputed.hard && std::fabs(s.score.soft - recomputed.soft) < 1e-9,
              "returned score matches a from-scratch recompute");

        // The final derived fields describe the final routes.
        bool shadowsOk = true;
        for (int t = 0; t < static_cast<int>(s.trolleys.size()); ++t) {
            const auto& list = s.trolleys[t].pickTaskIndices;
            for (int p = 0; p < static_cast<int>(list.size()); ++p) {
                const PickTask& task = s.pickTasks[list[p]];
                shadowsOk = shadowsOk && task.trolleyIndex == t &&
                            task.previousPickTask == (p > 0 ? list[p - 1] : -1) &&
                            task.nextPickTask ==
                                    (p + 1 < static_cast<int>(list.size()) ? list[p + 1] : -1);
            }
        }
        check(shadowsOk, "derived fields consistent on the final solution");
    }
}

// -------------------------------------------------------------------------
// (g) Layouts other than the default grid.
// -------------------------------------------------------------------------
bool refused(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

WarehouseLocation at(int column, int shelvingRow, Side side, int row) {
    WarehouseLocation location;
    location.column = column;
    location.shelvingRow = shelvingRow;
    location.side = side;
    location.row = row;
    return location;
}

bool tableGone(const OrderPickingSolution& s) {
    bool gone = s.warehouse.active == nullptr;
    if (s.warehouse.isGrid()) {
        for (const Trolley& trolley : s.trolleys) gone = gone && trolley.location.node == -1;
        for (const PickTask& task : s.pickTasks) gone = gone && task.location.node == -1;
    }
    return gone;
}

// One trolley, one order, three picks on a grid of 30 columns x 2 shelving
// rows, shelvings 4 m x 6 m, 2 m apart (6 m from a shelving to the next one,
// 8 m to the one below).
OrderPickingSolution customGridSolution() {
    OrderPickingSolution s;
    s.warehouse = makeGridWarehouse(30, 2, 4, 6, 2);
    s.orders.push_back({"order"});
    addTrolley(s, 4, 48000, at(0, 0, Side::LEFT, 0));
    addPick(s, 0, 1000, at(27, 1, Side::RIGHT, 3));  // (AB,2) x 166 y 11
    addPick(s, 0, 1000, at(27, 1, Side::LEFT, 5));   // (AB,2) x 162 y 13
    addPick(s, 0, 1000, at(26, 1, Side::RIGHT, 6));  // (AA,2) x 160 y 14
    return s;
}

// dock, a, b, c with an asymmetric table.
OrderPickingSolution namedSolution() {
    OrderPickingSolution s;
    s.warehouse = makeNamedWarehouse({"dock", "a", "b", "c"},
                                     {{0, 5, 9, 10}, {5, 0, 7, 4}, {9, 3, 0, 2}, {11, 4, 2, 0}});
    s.orders.push_back({"order"});
    addTrolley(s, 4, 48000, WarehouseLocation::named(0));
    for (int node = 1; node <= 3; ++node) addPick(s, 0, 1000, WarehouseLocation::named(node));
    return s;
}

void verifyLayouts() {
    std::printf("== (g) Layouts: a grid of another size, named locations ==\n");

    check(columnLetters(0) == "A" && columnLetters(25) == "Z" && columnLetters(26) == "AA" &&
                  columnLetters(701) == "ZZ" && columnLetters(702) == "AAA" &&
                  columnLetters(18277) == "ZZZ",
          "column letters count as spreadsheet columns do");
    check(at(26, 1, Side::LEFT, 0).shelvingId() == "(AA,2)", "shelving id beyond column Z");

    // The default-constructed warehouse is the grid of the two-argument
    // calculateDistance.
    {
        const Warehouse defaults;
        const WarehouseLocation from = loc('C', 1, Side::LEFT, 1);
        const WarehouseLocation to = loc('E', 3, Side::RIGHT, 3);
        check(defaults.isGrid() && calculateDistance(defaults, from, to) == 40 &&
                      calculateDistance(from, to) == 40,
              "Warehouse{} is the default grid (5 x 3, shelvings 2 x 10, padding 3)");
    }

    // A grid of another size, by hand: 177 + 8 + 3 out, 174 back.
    {
        OrderPickingSolution s = customGridSolution();
        const Warehouse& w = s.warehouse;
        const WarehouseLocation& start = s.trolleys[0].location;
        const WarehouseLocation& first = s.pickTasks[0].location;
        const WarehouseLocation& second = s.pickTasks[1].location;
        const WarehouseLocation& third = s.pickTasks[2].location;
        check(calculateDistance(w, start, first) == 177 &&
                      calculateDistance(w, first, start) == 177,
              "custom grid: another shelving row, 166 + 11");
        check(calculateDistance(w, first, second) == 8 && calculateDistance(w, second, first) == 8,
              "custom grid: the other side of the shelving, 4 + min(8, 4)");
        check(calculateDistance(w, second, third) == 3 && calculateDistance(w, third, second) == 3,
              "custom grid: facing sides across the corridor, 2 + 1");
        check(calculateDistance(w, third, start) == 174,
              "custom grid: back to the start, 160 + 14");
        s.trolleys[0].pickTaskIndices = {0, 1, 2};
        check(calculateDistanceToTravel(s, s.trolleys[0]) == 362, "custom grid: the walk is 362 m");
        const ConstraintTotals totals = ScoreCalculator::calculateTotals(s);
        check(totals.distanceFromPreviousPickTaskCost == 188 &&
                      totals.distanceFromLastPickTaskToPathOriginCost == 174,
              "custom grid: the two distance constraints, 188 + 174");
        OrderPickingIncrementalScoreCalculator incremental;
        incremental.rebuild(s);
        check(incremental.score().soft == -1362.0, "custom grid: incremental score -1362");
        checkLayout(s);
        check(refused([&] {
                  OrderPickingSolution inDefault = s;
                  inDefault.warehouse = Warehouse{};
                  checkLayout(inDefault);
              }),
              "the same locations are outside the default grid");
        check(refused([&] {
                  OrderPickingSolution bad = s;
                  bad.pickTasks[0].location.row = 7;
                  Solver().solve(bad);
              }),
              "a row beyond the shelving height is refused by the solve");
        check(refused([&] {
                  OrderPickingSolution bad = s;
                  bad.trolleys[0].location = WarehouseLocation::named(0);
                  checkLayout(bad);
              }),
              "a named location in a grid warehouse is refused");
    }
    check(refused([] { makeGridWarehouse(0); }) && refused([] { makeGridWarehouse(18279); }) &&
                  refused([] { makeGridWarehouse(5, 0); }) &&
                  refused([] { makeGridWarehouse(5, 3, 0); }) &&
                  refused([] { makeGridWarehouse(5, 3, 2, 0); }) &&
                  refused([] { makeGridWarehouse(5, 3, 2, 1001); }) &&
                  refused([] { makeGridWarehouse(5, 3, 2, 10, 0); }),
          "grid sizes out of range are refused");

    // Named locations, by hand.
    {
        OrderPickingSolution s = namedSolution();
        const Warehouse& w = s.warehouse;
        check(!w.isGrid() && locationLabel(w, s.pickTasks[1].location) == "b",
              "named locations carry their ids");
        check(calculateDistance(w, s.pickTasks[0].location, s.pickTasks[1].location) == 7 &&
                      calculateDistance(w, s.pickTasks[1].location, s.pickTasks[0].location) == 3,
              "named: an asymmetric pair reads both ways (a->b 7, b->a 3)");
        const std::vector<std::pair<std::vector<int>, int>> walks = {
                {{0, 1, 2}, 25}, {{0, 2, 1}, 20}, {{1, 0, 2}, 27},
                {{1, 2, 0}, 20}, {{2, 0, 1}, 30}, {{2, 1, 0}, 20}};
        bool walksOk = true;
        for (const auto& [order, walked] : walks) {
            s.trolleys[0].pickTaskIndices = order;
            walksOk = walksOk && calculateDistanceToTravel(s, s.trolleys[0]) == walked &&
                      ScoreCalculator::score(s).soft == -(walked + 1000.0);
        }
        check(walksOk, "named: the six walks of three stops, by hand");
        s.trolleys[0].pickTaskIndices = {0, 1, 2};
        const ConstraintTotals totals = ScoreCalculator::calculateTotals(s);
        check(totals.distanceFromPreviousPickTaskCost == 14 &&
                      totals.distanceFromLastPickTaskToPathOriginCost == 11,
              "named: the two distance constraints, 14 + 11");

        // Warm start: with no time to search the given route comes back.
        Solver::Config still;
        still.time_limit = std::chrono::seconds(0);
        Solver(still).solve(s);
        check(s.trolleys[0].pickTaskIndices == std::vector<int>({0, 1, 2}) &&
                      s.score.soft == -1025.0 && tableGone(s),
              "named: a warm start with no time comes back as given, -1025");

        // And a search from it, checked step by step, finds a 20 m walk.
        Solver::Config config;
        config.time_limit = std::chrono::milliseconds(300);
        config.score_checks = optimaru::ScoreChecks::Full;
        config.seed = 1;
        bool threw = false;
        try {
            Solver(config).solve(s);
        } catch (const optimaru::ScoreMismatchError&) {
            threw = true;
        }
        check(!threw && s.score.hard == 0 && s.score.soft == -1020.0 && tableGone(s),
              "named: FULL_ASSERT solve reaches the shortest walk, " + s.score.str());

        check(refused([&] {
                  OrderPickingSolution bad = s;
                  bad.pickTasks[0].location = at(0, 0, Side::LEFT, 0);
                  Solver().solve(bad);
              }),
              "a grid location among named locations is refused by the solve");
        check(refused([&] {
                  OrderPickingSolution bad = s;
                  bad.pickTasks[0].location = WarehouseLocation::named(4);
                  checkLayout(bad);
              }),
              "a location index beyond the table is refused");
        check(refused([&] {
                  calculateDistance(w, WarehouseLocation::named(9), s.trolleys[0].location);
              }),
              "outside a solve, a distance from an unknown location throws");

        // A one-way loop: the walk from one stop to another is the rest of the
        // lap, 8 m per step forward. Read transposed anywhere (the score, the
        // cheapest insertion of the order moves), the routes found would go
        // around backwards and cost several laps; forwards, each trolley
        // walks one lap of 56 m.
        const int stops = 7;
        std::vector<std::string> ids;
        std::vector<std::vector<int>> lap(stops, std::vector<int>(stops, 0));
        for (int from = 0; from < stops; ++from) {
            ids.push_back("stop" + std::to_string(from));
            for (int to = 0; to < stops; ++to) lap[from][to] = 8 * ((to - from + stops) % stops);
        }
        OrderPickingSolution loop;
        loop.warehouse = makeNamedWarehouse(ids, lap);
        for (int o = 0; o < 4; ++o) loop.orders.push_back({"o" + std::to_string(o)});
        // Two buckets a trolley and one per order: two orders on each.
        for (int t = 0; t < 2; ++t) addTrolley(loop, 2, 10000, WarehouseLocation::named(0));
        for (int p = 0; p < 16; ++p)
            addPick(loop, p % 4, 500, WarehouseLocation::named(1 + (p * 5) % 6));
        Solver::Config loopConfig;
        loopConfig.time_limit = std::chrono::seconds(1);
        loopConfig.score_checks = optimaru::ScoreChecks::Full;
        loopConfig.seed = 3;
        Solver(loopConfig).solve(loop);
        check(loop.score.hard == 0 && loop.score.soft == -(2 * 56 + 4 * 1000.0) &&
                      calculateDistanceToTravel(loop, loop.trolleys[0]) == 56 &&
                      calculateDistanceToTravel(loop, loop.trolleys[1]) == 56,
              "one-way loop: both trolleys walk one lap forwards, " + loop.score.str());
        loopConfig.parallel_solver_count = 2;
        for (Trolley& trolley : loop.trolleys) trolley.pickTaskIndices.clear();
        Solver(loopConfig).solve(loop);
        check(loop.score.soft == -(2 * 56 + 4 * 1000.0) && tableGone(loop),
              "one-way loop: the same with two parallel starts, " + loop.score.str());
    }
    check(refused([] { makeNamedWarehouse({}, {}); }) &&
                  refused([] { makeNamedWarehouse({"a", "a"}, {{0, 1}, {1, 0}}); }) &&
                  refused([] { makeNamedWarehouse({"a", "b"}, {{0, 1}}); }) &&
                  refused([] { makeNamedWarehouse({"a", "b"}, {{0, 1}, {1}}); }) &&
                  refused([] { makeNamedWarehouse({"a", "b"}, {{0, -1}, {1, 0}}); }) &&
                  refused([] { makeNamedWarehouse({"a", "b"}, {{0, 1000001}, {1, 0}}); }) &&
                  refused([] { makeNamedWarehouse({"a", "b"}, {{0, 1}, {1, 2}}); }),
          "a table that is not square, repeats an id or holds a bad distance is refused");

    // Custom grid: warm start and a FULL_ASSERT solve (several trolleys, so
    // the order-level moves run on it too).
    {
        OrderPickingSolution s = customGridSolution();
        s.trolleys[0].pickTaskIndices = {0, 1, 2};
        Solver::Config still;
        still.time_limit = std::chrono::seconds(0);
        Solver(still).solve(s);
        check(s.trolleys[0].pickTaskIndices == std::vector<int>({0, 1, 2}) &&
                      s.score.soft == -1362.0 && tableGone(s),
              "custom grid: a warm start with no time comes back as given, -1362");

        OrderPickingSolution wide;
        wide.warehouse = makeGridWarehouse(40, 6, 3, 14, 4);
        std::mt19937_64 rng(11);
        for (int o = 0; o < 6; ++o) wide.orders.push_back({"o" + std::to_string(o)});
        for (int t = 0; t < 3; ++t) addTrolley(wide, 5, 5000, at(0, 0, Side::LEFT, 0));
        for (int p = 0; p < 36; ++p) {
            addPick(wide, p % 6, 600 + static_cast<int>(rng() % 900),
                    at(static_cast<int>(rng() % 40), static_cast<int>(rng() % 6),
                       rng() % 2 == 0 ? Side::LEFT : Side::RIGHT, static_cast<int>(rng() % 15)));
        }
        Solver::Config config;
        config.time_limit = std::chrono::seconds(1);
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruin_recreate_unimproved_step_trigger = 100;
        config.ruin_recreate_polish_steps = 50;
        config.seed = 2;
        bool threw = false;
        try {
            Solver(config).solve(wide);
        } catch (const optimaru::ScoreMismatchError&) {
            threw = true;
        }
        OrderPickingSolution copy = wide;
        check(!threw && wide.score.feasible() && tableGone(wide) &&
                      ScoreCalculator::score(copy) == wide.score,
              "custom grid: 1s FULL_ASSERT solve, score == a recompute, " + wide.score.str());
    }

    // The solve's distance table holds what the grid arithmetic gives, and a
    // grid that uses more distinct locations than the table takes gets none
    // (every distance is then computed, as outside a solve).
    {
        const auto build = [](int pickCount) {
            OrderPickingSolution s;
            s.warehouse = makeGridWarehouse(60, 40, 2, 10, 3);
            for (int o = 0; o < 40; ++o) s.orders.push_back({"o" + std::to_string(o)});
            for (int t = 0; t < 12; ++t) addTrolley(s, 400, 5000, at(0, 0, Side::LEFT, 0));
            // Distinct locations: pick p at column p % 60, shelving row (p / 60) % 40, row p /
            // 2400.
            for (int p = 0; p < pickCount; ++p)
                addPick(s, p % 40, 100, at(p % 60, (p / 60) % 40, Side::LEFT, p / 2400));
            return s;
        };
        OrderPickingSolution large = build(ActiveDistances::kMaxGridNodes + 10);
        bool tableSkipped = false;
        {
            const ActiveDistances distances(large);
            tableSkipped = large.warehouse.active == nullptr;
        }
        OrderPickingSolution small = build(300);
        bool tableBuilt = false;
        {
            const ActiveDistances distances(small);
            tableBuilt = small.warehouse.active != nullptr && small.warehouse.active->size == 300 &&
                         small.pickTasks[7].location.node >= 0;
            bool same = true;
            for (int a = 0; a < 300; a += 7) {
                for (int b = 0; b < 300; b += 11) {
                    same = same &&
                           calculateDistance(small.warehouse, small.pickTasks[a].location,
                                             small.pickTasks[b].location) ==
                                   gridDistance(small.warehouse, small.pickTasks[a].location,
                                                small.pickTasks[b].location);
                }
            }
            tableBuilt = tableBuilt && same;
        }
        check(tableSkipped && tableGone(large), "a grid over the table's size limit gets no table");

        // A solve that ends in an exception (here: the callback's) leaves no
        // table and no node behind either, on a grid and on named locations.
        for (OrderPickingSolution thrown : {build(60), namedSolution()}) {
            Solver::Config throwing;
            throwing.time_limit = std::chrono::seconds(5);
            throwing.seed = 1;
            throwing.on_new_best = [](std::int64_t, const Score&) {
                throw std::runtime_error("stop");
            };
            bool threw = false;
            try {
                Solver(throwing).solve(thrown);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            check(threw && tableGone(thrown),
                  std::string(thrown.warehouse.isGrid() ? "grid" : "named") +
                          ": a solve that throws leaves no table behind");
        }

        // A solve of it runs on the grid arithmetic (from given routes: the
        // construction of two thousand picks is not what is tested here).
        for (int p = 0; p < static_cast<int>(large.pickTasks.size()); ++p)
            large.trolleys[p % 12].pickTaskIndices.push_back(p);
        const Score before = ScoreCalculator::score(large);
        Solver::Config limited;
        limited.time_limit = std::chrono::seconds(60);
        limited.step_limit = 2000;
        limited.seed = 4;
        Solver(limited).solve(large);
        OrderPickingSolution recomputed = large;
        check(large.score >= before && ScoreCalculator::score(recomputed) == large.score &&
                      tableGone(large),
              "a grid over the limit solves without a table, " + before.str() + " -> " +
                      large.score.str());

        // Parallel starts share one table, and leave none behind.
        OrderPickingSolution shared = build(300);
        Solver::Config parallel;
        parallel.time_limit = std::chrono::seconds(60);
        parallel.step_limit = 2000;
        parallel.parallel_solver_count = 3;
        parallel.seed = 4;
        Solver(parallel).solve(shared);
        OrderPickingSolution again = shared;
        check(shared.score.feasible() && ScoreCalculator::score(again) == shared.score &&
                      tableGone(shared),
              "three parallel starts on a grid: one shared table, gone afterwards, " +
                      shared.score.str());
        check(tableBuilt && tableGone(small),
              "the solve's table holds the grid distances and is gone afterwards");
    }
}

}  // namespace

int main() {
    verifyWarehouseDistances();
    verifyConstraints();
    verifyShadowVariables();
    verifyIncrementalAgainstFull();
    verifyDemoData();
    verifySolve();
    verifyLayouts();

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll checks passed\n");
    return 0;
}

// Bin-packing unit tests (engine test/ style: check() + non-zero exit on any
// failure):
//   (a) every constraint verified in isolation against hand-computed scores,
//       a violating (positive) and a clean (zero) case each, through the
//       monolithic ScoreCalculator AND the incremental calculator, at the
//       components' kDefaultWeight (all 1)
//   (b) incremental score == full recompute across a long random walk of
//       change / swap / unassign moves driven through the engine's
//       ScoreDirector protocol, with colours and (duplicated) conflicts:
//       the score and each constraint's raw total after every move, every
//       constraint non-zero during the walk; once with the flat colour
//       counts and once with the hash-map fallback
//   (c) the construction reproduces 0.1.5's best fit (plain, partial seed,
//       gapped seed) and stays inside the value range where 0.1.5 did not
//   (d) zero budget: a complete gapped seed comes back unchanged; a plain
//       solve ignores the input bins and is deterministic under a step
//       limit; warmStart is cleared by every solve
//   (e) a small coloured / conflicting instance solved under FULL_ASSERT,
//       with EmptyBinMove and the ALNS kicks on
//   (f) the 20-item samples reach the 8-bin lower bound at 0hard
//   (g) the parallel multi-start returns a consistent best solution
//   (h) compaction is score-neutral and leaves contiguous labels
//   (i) the fact checks (capacity, ids, weights, conflicts, bin range)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bp/construction.hpp"
#include "bp/domain.hpp"
#include "bp/incremental_calculator.hpp"
#include "bp/score_calculator.hpp"
#include "bp/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace bp;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

bool sameScore(const Score &a, const Score &b)
{
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

// Both monolithic calculators must agree with the hand calculation.
void checkScore(const std::string &label,
                BinPackingProblem problem,
                long expectedHard,
                double expectedSoft)
{
    const Score full = ScoreCalculator::calculateScore(problem);
    BinPackingIncrementalScoreCalculator incremental;
    incremental.resetWorkingSolution(problem);
    const Score inc = incremental.calculateScore();
    const bool ok = full.hard == expectedHard && std::fabs(full.soft - expectedSoft) < 1e-9
                    && sameScore(full, inc);
    std::printf("  [%s] %-58s full=%s incremental=%s expected=%ldhard/%gsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                full.toString().c_str(),
                inc.toString().c_str(),
                expectedHard,
                expectedSoft);
    if (!ok)
        ++failures;
}

Item makeItem(const std::string &id, std::int64_t weight, int bin = -1, const char *color = nullptr)
{
    Item item;
    item.id = id;
    item.weight = weight;
    item.bin = bin;
    if (color != nullptr)
        item.color = std::string(color);
    return item;
}

// C = 10, A8 B7 C3 D2 (the 0.1.5 hand-calculation fixture).
BinPackingProblem abcd(const std::vector<int> &bins)
{
    BinPackingProblem p;
    p.binCapacity = 10;
    p.items = {makeItem("A", 8), makeItem("B", 7), makeItem("C", 3), makeItem("D", 2)};
    p.setAssignment(bins);
    return p;
}

std::vector<int> binsOf(const BinPackingProblem &p) { return p.assignment(); }

// -------------------------------------------------------------------------
// (a) hand-calculated positive and zero cases
// -------------------------------------------------------------------------
void verifyConstraintsByHand()
{
    std::printf("== (a) hand-calculated constraint scores ==\n");

    // binCapacityCost: {A8, B7} = 15 on capacity 10 -> 5; {C3, D2} -> 0.
    // usedBinsCost: 2 bins in use.
    checkScore("binCapacityCost positive: {A,B} 15/10 -> 5", abcd({0, 0, 1, 1}), -5, -2.0);
    checkScore("binCapacityCost zero: {A,D} {B,C} exactly full", abcd({0, 1, 1, 0}), 0, -2.0);
    checkScore("usedBinsCost positive: 4 singleton bins", abcd({0, 1, 2, 3}), 0, -4.0);
    checkScore("usedBinsCost zero: everything unassigned", abcd({-1, -1, -1, -1}), 0, 0.0);
    {
        BinPackingProblem empty;
        empty.binCapacity = 10;
        checkScore("usedBinsCost zero: no items", empty, 0, 0.0);
    }

    // colorConflictCost.
    {
        BinPackingProblem p;
        p.binCapacity = 100;
        p.items = {makeItem("A", 3, 0, "red"), makeItem("B", 4, 0, "red"), makeItem("C", 5, 0, "blue")};
        checkScore("colorConflictCost positive: red red blue -> 1", p, -1, -1.0);
        p.items[1].bin = 1;
        checkScore("colorConflictCost zero: the reds split", p, 0, -2.0);
    }
    {
        BinPackingProblem p;
        p.binCapacity = 100;
        p.items = {makeItem("R1", 1, 0, "red"),
                   makeItem("R2", 1, 0, "red"),
                   makeItem("R3", 1, 0, "red"),
                   makeItem("B1", 1, 0, "blue"),
                   makeItem("B2", 1, 0, "blue"),
                   makeItem("X", 1, 0)};
        checkScore("colorConflictCost positive: red x3 + blue x2 -> 2 + 1", p, -3, -1.0);
        for (Item &item : p.items)
            item.color.reset();
        checkScore("colorConflictCost zero: no colours", p, 0, -1.0);
    }

    // itemConflictCost.
    {
        BinPackingProblem p;
        p.binCapacity = 100;
        p.items = {makeItem("A", 3, 0), makeItem("B", 4, 1), makeItem("C", 5, 0)};
        p.conflicts = {{"A", "C"}};
        checkScore("itemConflictCost positive: A-C together -> 1", p, -1, -2.0);
        p.items[2].bin = 1;
        checkScore("itemConflictCost zero: A-C apart", p, 0, -2.0);
        p.items[2].bin = 0;
        p.conflicts = {{"A", "C"}, {"C", "A"}};
        p.prepared = false;  // the facts changed
        checkScore("itemConflictCost positive: (A,C) + (C,A) together -> 2", p, -2, -2.0);
        p.items[0].bin = -1;
        checkScore("itemConflictCost zero: one endpoint unassigned", p, 0, -2.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) incremental == full recompute over a random walk
// -------------------------------------------------------------------------
BinPackingProblem randomInstance(int n, std::uint64_t seed, int colors, int conflicts)
{
    std::mt19937_64 rng(seed);
    BinPackingProblem p;
    p.binCapacity = 20;
    for (int i = 0; i < n; ++i)
    {
        Item item = makeItem("i" + std::to_string(i), 1 + static_cast<std::int64_t>(rng() % 12));
        if (colors > 0 && rng() % 3 != 0)
            item.color = "c" + std::to_string(rng() % colors);
        item.bin = static_cast<int>(rng() % n);
        p.items.push_back(item);
    }
    for (int k = 0; k < conflicts; ++k)
    {
        const int a = static_cast<int>(rng() % n);
        int b = static_cast<int>(rng() % (n - 1));
        if (b >= a)
            ++b;
        p.conflicts.emplace_back("i" + std::to_string(a), "i" + std::to_string(b));
        if (k % 5 == 0)  // duplicated and reversed listings count too
            p.conflicts.emplace_back("i" + std::to_string(b), "i" + std::to_string(a));
    }
    return p;
}

// The four raw totals as a list, in catalog order.
std::vector<std::int64_t> asList(const RawCosts &raw)
{
    return {raw.binCapacity, raw.colorConflict, raw.itemConflict, raw.usedBins};
}

const char *const kConstraintNames[] = {"binCapacityCost", "colorConflictCost", "itemConflictCost",
                                        "usedBinsCost"};

// One walk: after every move the incremental score AND each constraint's
// running raw total must equal the full recompute's, and every constraint
// must be non-zero at some point (so none of the four is compared at 0 only).
void walkIncremental(std::size_t maxFlatColorCells, const std::string &label)
{
    BinPackingProblem p = randomInstance(30, 7, 4, 25);
    BinPackingIncrementalScoreCalculator calc(maxFlatColorCells);
    optimaru::ScoreDirector<BinPackingProblem> director(p, calc);
    BinPackingProblem shadow = p;
    check(calc.flatColorCounts() == (maxFlatColorCells != 0),
          label + ": the colour counts use the intended storage");

    std::mt19937_64 rng(123);
    const int n = p.itemCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int mismatches = 0;
    int costMismatches = 0;
    int nonZero[4] = {0, 0, 0, 0};
    const int steps = 5000;
    for (int step = 0; step < steps; ++step)
    {
        const double r = unit(rng);
        if (r < 0.6)
        {
            // ChangeMove; 10% of the time to unassigned (-1).
            const int item = static_cast<int>(rng() % n);
            const int bin = unit(rng) < 0.10 ? -1 : static_cast<int>(rng() % 6);
            director.changeVariable(item, bin);
        }
        else if (r < 0.9)
        {
            const int a = static_cast<int>(rng() % n);
            const int b = static_cast<int>(rng() % n);
            const int va = p.items[a].bin;
            const int vb = p.items[b].bin;
            director.beginVariableChanges();
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
            director.endVariableChanges();
        }
        else
        {
            // Move a whole bin's items to another bin (EmptyBinMove's shape).
            const int from = static_cast<int>(rng() % 6);
            const int to = static_cast<int>(rng() % 6);
            director.beginVariableChanges();
            for (int i = 0; i < n; ++i)
            {
                if (p.items[i].bin == from)
                    director.changeVariable(i, to);
            }
            director.endVariableChanges();
        }
        shadow.setAssignment(p.assignment());
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = director.calculateScore();
        if (!sameScore(full, incremental) && ++mismatches <= 3)
        {
            std::printf("  MISMATCH @step %d: incremental=%s full=%s\n",
                        step,
                        incremental.toString().c_str(),
                        full.toString().c_str());
        }
        const std::vector<std::int64_t> expected = asList(ScoreCalculator::rawCosts(shadow));
        const std::vector<std::int64_t> running = asList(calc.totals());
        for (int c = 0; c < 4; ++c)
        {
            if (running[c] != expected[c] && ++costMismatches <= 3)
            {
                std::printf("  COST MISMATCH @step %d: %s incremental=%lld full=%lld\n",
                            step,
                            kConstraintNames[c],
                            static_cast<long long>(running[c]),
                            static_cast<long long>(expected[c]));
            }
            if (expected[c] != 0)
                ++nonZero[c];
        }
    }
    check(mismatches == 0, label + ": " + std::to_string(steps) + " random moves, incremental == full recompute");
    check(costMismatches == 0, label + ": every constraint's raw total == the full recompute's after each move");
    for (int c = 0; c < 4; ++c)
    {
        check(nonZero[c] > 0,
              label + ": " + kConstraintNames[c] + " non-zero at " + std::to_string(nonZero[c]) + " of "
                  + std::to_string(steps) + " steps");
    }
}

void verifyIncremental()
{
    std::printf("== (b) incremental scoring == full recompute, per constraint ==\n");
    walkIncremental(BinPackingIncrementalScoreCalculator::kDefaultMaxFlatColorCells, "flat colour counts");
    walkIncremental(0, "hash-map colour counts");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) construction == 0.1.5 best fit
// -------------------------------------------------------------------------
std::vector<int> constructed(BinPackingProblem p)
{
    ScoreCalculator::prepare(p);
    constructBins(p, descendingOrder(p));
    return binsOf(p);
}

void verifyConstruction()
{
    std::printf("== (c) construction ==\n");
    check(constructed(abcd({-1, -1, -1, -1})) == std::vector<int>{0, 1, 1, 0},
          "plain: A:0 B:1 C:1 D:0 (0.1.5)");
    check(constructed(abcd({-1, 0, -1, 0})) == std::vector<int>{1, 0, 2, 0},
          "partial seed {B:0, D:0}: A:1, C:2 (0.1.5)");
    check(constructed(abcd({3, -1, -1, -1})) == std::vector<int>{3, 0, 0, 3},
          "gapped seed {A:3}: B:0, C:0, D:3 (0.1.5)");
    {
        // n = 2, A seeded at 1, B heavier than the capacity: 0.1.5 opened
        // bin 2 (out of range); the port opens the lowest empty bin.
        BinPackingProblem p;
        p.binCapacity = 5;
        p.items = {makeItem("A", 2, 1), makeItem("B", 9)};
        check(constructed(p) == std::vector<int>{1, 0}, "seed {A:1}, B fits nowhere: B:0, inside 0..n-1");
    }
    {
        BinPackingProblem p = abcd({-1, -1, -1, -1});
        p.items[3].weight = 8;  // A8 B7 C3 D8: ties between A and D
        std::mt19937_64 rng(5);
        bool tiesOnly = true;
        for (int trial = 0; trial < 20; ++trial)
        {
            const std::vector<int> order = tieShuffledOrder(p, rng);
            tiesOnly &= order.size() == 4 && order[2] == 1 && order[3] == 2
                        && ((order[0] == 0 && order[1] == 3) || (order[0] == 3 && order[1] == 0));
        }
        check(tiesOnly, "tieShuffledOrder only shuffles items of equal weight");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) zero budget and warm start
// -------------------------------------------------------------------------
Solver::Config zeroBudget()
{
    Solver::Config config;
    config.spentLimit = std::chrono::milliseconds(0);
    return config;
}

void verifyZeroBudgetAndWarmStart()
{
    std::printf("== (d) zero budget, warm start ==\n");
    {
        BinPackingProblem p = abcd({0, 1, 2, 2});
        p.warmStart = true;
        Solver(zeroBudget()).solve(p);
        check(binsOf(p) == std::vector<int>{0, 1, 2, 2} && p.binsUsed() == 3,
              "complete seed comes back unchanged with zero budget");
        check(!p.warmStart, "warmStart is cleared by the solve");
    }
    {
        BinPackingProblem p = abcd({3, 1, 1, 3});
        p.warmStart = true;
        Solver(zeroBudget()).solve(p);
        check(binsOf(p) == std::vector<int>{3, 1, 1, 3}, "gapped seed keeps its gaps with zero budget");
    }
    {
        BinPackingProblem p = abcd({0, 0, 0, 0});
        p.warmStart = true;
        Solver(zeroBudget()).solve(p);
        check(binsOf(p) == std::vector<int>{0, 0, 0, 0} && p.score.hard == -10,
              "overloaded seed comes back unchanged (-10hard)");
    }
    {
        BinPackingProblem p = abcd({0, 0, 0, 0});  // warmStart false: ignored
        Solver(zeroBudget()).solve(p);
        check(binsOf(p) == std::vector<int>{0, 1, 1, 0}, "plain zero-budget solve = the construction");
    }
    {
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(30);
        config.stepCountLimit = 2000;
        config.randomSeed = 5;
        BinPackingProblem fresh = randomInstance(16, 3, 3, 6);
        for (Item &item : fresh.items)
            item.bin = -1;
        BinPackingProblem preset = fresh;
        for (Item &item : preset.items)
            item.bin = 0;
        Solver(config).solve(fresh);
        Solver(config).solve(preset);
        check(binsOf(fresh) == binsOf(preset) && sameScore(fresh.score, preset.score),
              "plain solve ignores input bins and is deterministic under a step limit");
    }
    {
        BinPackingProblem p;
        p.binCapacity = 10;
        p.warmStart = true;
        Solver(zeroBudget()).solve(p);
        check(!p.warmStart && sameScore(p.score, Score{}), "no items: 0hard/0soft, warmStart cleared");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) FULL_ASSERT
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (e) FULL_ASSERT solve ==\n");
    BinPackingProblem p = randomInstance(18, 11, 3, 8);
    for (Item &item : p.items)
        item.bin = -1;
    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    // Kick early so the LNS ruin/recreate path also runs under FULL_ASSERT.
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;
    config.ruinRecreateAsMoveWeight = 0.05;
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
    check(!corrupted, "2s FULL_ASSERT solve without score corruption");
    if (!corrupted)
    {
        const Score reported = p.score;
        check(sameScore(reported, ScoreCalculator::calculateScore(p)),
              "solver-reported score == full recompute");
        check(std::all_of(p.items.begin(), p.items.end(), [](const Item &item) { return item.bin >= 0; }),
              "every item assigned");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) the samples reach the lower bound
// -------------------------------------------------------------------------
BinPackingProblem twentyItems(bool colored)
{
    const std::int64_t weights[] = {48, 30, 19, 36, 36, 27, 42, 42, 42, 42,
                                    42, 42, 24, 24, 51, 49, 55, 45, 33, 17};
    const char *colors[] = {"red", "blue", "green", "yellow", nullptr, nullptr, "red",
                            "blue", "green", "yellow", nullptr, nullptr, nullptr, nullptr,
                            "red", "blue", nullptr, nullptr, nullptr, nullptr};
    BinPackingProblem p;
    p.binCapacity = 100;
    for (int i = 0; i < 20; ++i)
    {
        char id[8];
        std::snprintf(id, sizeof(id), "I%02d", i + 1);
        p.items.push_back(makeItem(id, weights[i], -1, colored ? colors[i] : nullptr));
    }
    if (colored)
        p.conflicts = {{"I01", "I16"}, {"I07", "I18"}, {"I10", "I19"}};
    return p;
}

void verifySamples()
{
    std::printf("== (f) samples reach the 8-bin lower bound ==\n");
    for (bool colored : {false, true})
    {
        BinPackingProblem p = twentyItems(colored);
        std::int64_t total = 0;
        for (const Item &item : p.items)
            total += item.weight;
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(5);
        config.bestScoreLimit = Score{0, -8.0};
        config.randomSeed = 7;
        Solver(config).solve(p);
        check(total == 746 && p.score.hard == 0 && p.binsUsed() == 8 && p.score.soft == -8.0,
              std::string(colored ? "web demo (colours + conflicts)" : "20 weights, capacity 100")
                  + ": 0hard/-8soft, 8 bins (got " + p.score.toString() + ")");
        const std::vector<std::int64_t> loads = p.binLoads();
        check(loads.size() == 8 && std::all_of(loads.begin(), loads.end(), [](std::int64_t l) { return l > 0; }),
              "bin_loads has no gaps after the search (compacted)");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) parallel multi-start
// -------------------------------------------------------------------------
void verifyParallel()
{
    std::printf("== (g) parallel multi-start ==\n");
    BinPackingProblem p = twentyItems(true);
    Solver::Config config;
    config.spentLimit = std::chrono::milliseconds(500);
    config.parallelSolverCount = 3;
    config.randomSeed = 11;
    Solver(config).solve(p);
    const Score reported = p.score;
    check(sameScore(reported, ScoreCalculator::calculateScore(p)), "best worker's score == full recompute");
    check(std::all_of(p.items.begin(), p.items.end(), [](const Item &item) { return item.bin >= 0; }),
          "every item assigned");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) compaction
// -------------------------------------------------------------------------
void verifyCompaction()
{
    std::printf("== (h) compaction ==\n");
    BinPackingProblem p = randomInstance(20, 21, 3, 10);
    const Score before = ScoreCalculator::calculateScore(p);
    compactBins(p);
    const Score after = ScoreCalculator::calculateScore(p);
    check(sameScore(before, after), "compaction is score-neutral");
    int expected = 0;
    bool contiguous = true;
    std::vector<char> seen(p.items.size(), 0);
    for (const Item &item : p.items)
    {
        if (!seen[item.bin])
        {
            contiguous &= item.bin == expected++;
            seen[item.bin] = 1;
        }
    }
    check(contiguous, "labels are 0..k-1 by first appearance");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) the fact checks
// -------------------------------------------------------------------------
bool throwsInvalid(const std::function<void()> &action, const std::string &needle)
{
    try
    {
        action();
    }
    catch (const std::invalid_argument &e)
    {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}

void verifyChecks()
{
    std::printf("== (i) fact checks ==\n");
    check(throwsInvalid(
              []
              {
                  BinPackingProblem p = abcd({-1, -1, -1, -1});
                  p.binCapacity = 0;
                  Solver(zeroBudget()).solve(p);
              },
              "bin_capacity must be positive"),
          "solve refuses binCapacity <= 0");
    check(throwsInvalid(
              []
              {
                  BinPackingProblem p = abcd({0, 0, 1, -1});
                  p.binCapacity = -3;
                  ScoreCalculator::calculateScore(p);
              },
              "bin_capacity must be positive"),
          "scoring refuses binCapacity <= 0");
    check(throwsInvalid(
              []
              {
                  BinPackingProblem p = abcd({0, 0, 1, 1});
                  ScoreCalculator::prepare(p);
                  p.binCapacity = 0;  // edited after the lookups were built
                  BinPackingIncrementalScoreCalculator incremental;
                  incremental.resetWorkingSolution(p);
              },
              "bin_capacity must be positive"),
          "a capacity edited after prepare() is refused too");
    check(throwsInvalid(
              []
              {
                  BinPackingProblem p = abcd({-1, -1, -1, -1});
                  p.items[1].id = "A";
                  ScoreCalculator::calculateScore(p);
              },
              "item ids must be unique"),
          "a repeated id is refused");
    check(throwsInvalid(
              []
              {
                  BinPackingProblem p = abcd({-1, -1, -1, -1});
                  p.items[1].weight = 0;
                  ScoreCalculator::calculateScore(p);
              },
              "item weights must be positive"),
          "a weight <= 0 is refused");
    check(throwsInvalid(
              []
              {
                  BinPackingProblem p = abcd({-1, -1, -1, -1});
                  p.conflicts = {{"A", "Z"}};
                  ScoreCalculator::calculateScore(p);
              },
              "conflict endpoints"),
          "an unknown conflict endpoint is refused");
    check(throwsInvalid(
              []
              {
                  BinPackingProblem p = abcd({-1, -1, -1, -1});
                  p.conflicts = {{"A", "A"}};
                  ScoreCalculator::calculateScore(p);
              },
              "conflict endpoints"),
          "a self pair is refused");
    check(throwsInvalid(
              []
              {
                  BinPackingProblem p = abcd({0, 0, 0, 4});
                  ScoreCalculator::calculateScore(p);
              },
              "item 'D' has bin 4, outside -1..3"),
          "a bin outside -1..n-1 is refused");
    {
        BinPackingProblem p = abcd({0, 0, 0, 7});  // a plain solve ignores the bins
        Solver(zeroBudget()).solve(p);
        check(binsOf(p) == std::vector<int>{0, 1, 1, 0}, "a plain solve ignores out-of-range input bins");
    }
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraintsByHand();
    verifyIncremental();
    verifyConstruction();
    verifyZeroBudgetAndWarmStart();
    verifyFullAssertSolve();
    verifySamples();
    verifyParallel();
    verifyCompaction();
    verifyChecks();
    if (failures != 0)
    {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all bp checks passed\n");
    return 0;
}

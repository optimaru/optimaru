// Monolithic-path tests, engine test/ style: check() + a non-zero exit on any
// failure.
//   (1) hand-calculated scores, each on the full recompute AND the
//       incremental calculator: a positive and a zero case per constraint
//   (2) incremental == full recompute across random flips, swaps and -1
//       (undecided) transitions on random instances using every fact: the
//       score and every constraint's raw total after each move, and every
//       constraint non-zero at some point of the walk
//   (3) the greedy construction (the reference expectations, ties, fixed items)
//   (4) the solver: ScoreChecks::Full, brute-force optima, the zero budget with a
//       seed, reproducibility under a step limit, multi-start, validation
//   (5) CoreRepackMove (ks/moves.hpp): a move is worth more and within the
//       capacity, incremental == full recompute after it and after its undo,
//       and the dynamic-programming optimum of plain knapsacks that the
//       built-in moves leave short of
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "ks/construction.hpp"
#include "ks/domain.hpp"
#include "ks/incremental_calculator.hpp"
#include "ks/moves.hpp"
#include "ks/score_calculator.hpp"
#include "ks/solver.hpp"
#include "ks_fixtures.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/search/operators.hpp"

using namespace ks;
using namespace ks_test;

namespace {

// The full recompute and a freshly reset incremental calculator on the
// current selection; checks that they agree and returns the full one.
Score scoreBothWays(KnapsackProblem& problem, const std::string& label) {
    const Score full = ScoreCalculator::score(problem);
    KnapsackIncrementalScoreCalculator incremental;
    incremental.rebuild(problem);
    const Score fast = incremental.score();
    if (!sameScore(full, fast)) {
        check(false, label + ": incremental " + fast.str() + " != full " + full.str());
    }
    return full;
}

void expectScore(KnapsackProblem& problem, std::int64_t hard, double soft,
                 const std::string& label) {
    const Score score = scoreBothWays(problem, label);
    check(score.hard == hard && std::fabs(score.soft - soft) < 1e-9, label + " -> " + score.str());
}

// -------------------------------------------------------------------------
// (1) Hand-calculated scores.
// -------------------------------------------------------------------------
void verifyHandCalculated() {
    std::printf("== (1) Hand-calculated scores (full and incremental) ==\n");
    {
        // A + C: weight 11 on capacity 10 -> 1 over; value 9 + 8 = 17.
        KnapsackProblem p = basicInstance();
        select(p, {0, 2});
        expectScore(p, -1, 17.0, "knapsackCapacityCost positive: A+C is 1 over, value 17");
        // B + C: weight 9 fits; value 15.
        select(p, {1, 2});
        expectScore(p, 0, 15.0, "knapsackCapacityCost zero: B+C fits, value 15");
        // A + B: weight exactly 10 fits; value 16.
        select(p, {0, 1});
        expectScore(p, 0, 16.0, "knapsackCapacityCost zero at exactly the capacity");
        // A + B + C: 15 on 10 -> 5 over.
        select(p, {0, 1, 2});
        expectScore(p, -5, 24.0, "A+B+C overweight: -5hard");
        // Nothing selected: no penalty, no reward.
        select(p, {});
        expectScore(p, 0, 0.0, "selectedValueCost zero: nothing selected scores 0soft");
    }
    {
        // Extended instance, {A}: labor 3 on 2 -> 1; mandatory C missing -> 1;
        // forbidden A selected -> 1; group {A,B,C} wants 2, has 1 -> 1;
        // A requires B, B not selected -> 1; weight 3 fits; value 5.
        KnapsackProblem p = extendedInstance();
        select(p, {0});
        expectScore(p, -5, 5.0, "extended {A}: five hard terms of 1 each, value 5");
        // {B, C}: every hard term 0; value 4 + 1 = 5.
        select(p, {1, 2});
        expectScore(p, 0, 5.0, "extended {B, C}: every hard term 0, value 5");
        // {A, B, C}: labor 3 -> 1, forbidden A -> 1, group 3 > 2 -> 1.
        select(p, {0, 1, 2});
        expectScore(p, -3, 10.0, "extended {A, B, C}: resource 1 + forbidden 1 + group excess 1");
    }
    {
        // Group above its maximum: max 1, two selected -> 1. Without a
        // maximum (= the group size) the same selection costs nothing.
        KnapsackProblem p = basicInstance();
        p.capacity = 100;
        SelectionGroup group;
        group.id = "pair";
        group.items = {0, 1};
        group.maxSelected = 1;
        p.selectionGroups.push_back(group);
        select(p, {0, 1});
        expectScore(p, -1, 16.0, "selectionGroupBoundsCost positive: 2 selected, max 1");
        p.selectionGroups[0].maxSelected.reset();
        expectScore(p, 0, 16.0, "selectionGroupBoundsCost zero: no maximum, no excess");
        // Below the minimum by 2.
        p.selectionGroups[0].minSelected = 2;
        select(p, {});
        expectScore(p, -2, 0.0, "selectionGroupBoundsCost positive: min 2, none selected");
    }
    {
        // mandatory [C, C] with C unselected -> 2; requires [B, B] with A
        // selected and B not -> 2.
        KnapsackProblem p = basicInstance();
        p.capacity = 100;
        p.mandatoryItems = {2, 2};
        p.items[0].requiredItems = {1, 1};
        select(p, {0});
        expectScore(p, -4, 9.0, "mandatory [C, C] missing -> 2, requires [B, B] unmet -> 2");
        select(p, {0, 1, 2});
        expectScore(p, 0, 24.0, "mandatoryItemsCost / itemDependencyCost zero: all selected");
    }
    {
        // forbidden [B, B] selected -> 2; zero once B is dropped.
        KnapsackProblem p = basicInstance();
        p.capacity = 100;
        p.forbiddenItems = {1, 1};
        select(p, {1});
        expectScore(p, -2, 7.0, "forbiddenItemsCost positive: forbidden [B, B] selected -> 2");
        select(p, {0, 2});
        expectScore(p, 0, 17.0, "forbiddenItemsCost zero: B not selected");
    }
    {
        // A self-dependency never counts; usage of an undeclared resource is
        // ignored; a dependency on an unselected item costs nothing while
        // the dependent item is unselected.
        KnapsackProblem p = basicInstance();
        p.capacity = 100;
        p.resources.push_back({"labor", 1});
        p.items[0].requiredItems = {0};
        p.items[0].resourceUsage["undeclared"] = 50;
        p.items[1].requiredItems = {2};
        p.items[1].resourceUsage["labor"] = 1;
        select(p, {0, 1, 2});
        expectScore(p, 0, 24.0, "self-dependency 0, undeclared resource ignored, labor 1 on 1");
        p.items[2].resourceUsage["labor"] = 2;
        expectScore(p, -2, 24.0, "resourceCapacityCost positive: labor 3 on 1 -> 2");
        select(p, {0});
        expectScore(p, 0, 9.0, "itemDependencyCost zero: B unselected, so B->C is no violation");
    }
    {
        // Two resources over by 2 and 3 -> 5; undecided items count as not
        // selected.
        KnapsackProblem p = basicInstance();
        p.capacity = 100;
        p.resources.push_back({"r1", 1});
        p.resources.push_back({"r2", 0});
        p.items[0].resourceUsage = {{"r1", 3}, {"r2", 1}};
        p.items[1].resourceUsage = {{"r2", 2}};
        select(p, {0, 1});
        expectScore(p, -5, 16.0, "two resources over by 2 and 3 -> 5");
        p.items[1].selected = kUndecided;
        p.items[2].selected = kUndecided;
        expectScore(p, -3, 9.0, "undecided items count as not selected");
    }
    {
        KnapsackProblem p;
        p.capacity = 5;
        expectScore(p, 0, 0.0, "no items: 0hard/0soft");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (2) Incremental == full recompute on a random walk, per constraint.
// -------------------------------------------------------------------------
// The raw totals in catalog order (defaultConstraints()), the selected value
// as the reward's raw cost (-value).
std::vector<std::int64_t> catalogOrder(const RawCosts& raw) {
    return {raw.capacityOverflow,
            raw.resourceOverflow,
            raw.missingMandatory,
            raw.chosenForbidden,
            raw.groupViolation,
            raw.unmetDependencies,
            -raw.value};
}

const char* const kCatalogNames[] = {"knapsackCapacityCost",     "resourceCapacityCost",
                                     "mandatoryItemsCost",       "forbiddenItemsCost",
                                     "selectionGroupBoundsCost", "itemDependencyCost",
                                     "selectedValueCost"};

void verifyIncrementalWalk() {
    std::printf("== (2) Incremental == full recompute on random walks, per constraint ==\n");
    int mismatches = 0;
    int costMismatches = 0;
    int nonZero[7] = {0, 0, 0, 0, 0, 0, 0};
    int steps = 0;
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        KnapsackProblem p = randomInstance(12 + static_cast<int>(seed) * 4, seed, 0.1);
        ScoreCalculator::prepare(p);
        KnapsackIncrementalScoreCalculator calculator;
        optimaru::Editor<KnapsackProblem> editor(p, calculator);
        KnapsackProblem shadow = p;
        std::mt19937_64 rng(seed * 101);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const int n = p.itemCount();
        for (int step = 0; step < 500; ++step, ++steps) {
            const double u = unit(rng);
            if (u < 0.6) {
                const int item = static_cast<int>(rng() % n);
                const int value = unit(rng) < 0.15 ? kUndecided : static_cast<int>(rng() % 2);
                editor.assign(item, value);
            } else {
                // A swap (or a random 3-item batch) as one composite change.
                editor.begin_batch();
                const int a = static_cast<int>(rng() % n);
                const int b = static_cast<int>(rng() % n);
                const int va = p.items[a].selected;
                editor.assign(a, p.items[b].selected);
                editor.assign(b, va);
                if (u > 0.9)
                    editor.assign(static_cast<int>(rng() % n), static_cast<int>(rng() % 2));
                editor.end_batch();
            }
            for (int i = 0; i < n; ++i) shadow.items[i].selected = p.items[i].selected;
            const Score full = ScoreCalculator::score(shadow);
            const Score incremental = editor.score();
            if (!sameScore(full, incremental) && ++mismatches <= 3) {
                std::printf("  MISMATCH seed %llu step %d: incremental=%s full=%s\n",
                            static_cast<unsigned long long>(seed), step, incremental.str().c_str(),
                            full.str().c_str());
            }
            const std::vector<std::int64_t> expected =
                    catalogOrder(ScoreCalculator::rawCosts(shadow));
            const std::vector<std::int64_t> running = catalogOrder(calculator.totals());
            for (int c = 0; c < 7; ++c) {
                if (running[c] != expected[c] && ++costMismatches <= 3) {
                    std::printf(
                            "  RAW COST MISMATCH seed %llu step %d %s: incremental=%lld "
                            "full=%lld\n",
                            static_cast<unsigned long long>(seed), step, kCatalogNames[c],
                            static_cast<long long>(running[c]),
                            static_cast<long long>(expected[c]));
                }
                if (expected[c] != 0) ++nonZero[c];
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps) + " random moves, incremental score == full recompute");
    check(costMismatches == 0,
          std::to_string(steps) +
                  " random moves, every constraint's incremental raw total == full recompute");
    for (int c = 0; c < 7; ++c) {
        check(nonZero[c] > 0, std::string(kCatalogNames[c]) + " is non-zero on " +
                                      std::to_string(nonZero[c]) + " of the walk's states");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (3) The greedy construction.
// -------------------------------------------------------------------------
void verifyGreedy() {
    std::printf("== (3) Greedy construction ==\n");
    KnapsackProblem p = basicInstance();
    ScoreCalculator::prepare(p);
    // Densities A 1.5, B 1.75, C 1.6: B (4), C (9), A would make 15 > 10.
    check(greedySelection(p, {-1, -1, -1}) == std::vector<int>{0, 1, 1}, "all undecided -> {B, C}");
    // A fixed in: load 6, B fits (10), C does not.
    check(greedySelection(p, {1, -1, -1}) == std::vector<int>{1, 1, 0}, "A fixed in -> {A, B}");
    // B fixed out: C (5), A would make 11.
    check(greedySelection(p, {-1, 0, -1}) == std::vector<int>{0, 0, 1}, "B fixed out -> {C}");
    // A complete decision is returned as given, even overweight.
    check(greedySelection(p, {1, 1, 1}) == std::vector<int>{1, 1, 1},
          "complete seed kept as given");

    // Equal densities keep item order: X, Y, Z all density 2, room for two.
    KnapsackProblem ties;
    ties.capacity = 4;
    ties.items = {makeItem("X", 2, 4), makeItem("Y", 2, 4), makeItem("Z", 2, 4)};
    ScoreCalculator::prepare(ties);
    check(greedySelection(ties, {-1, -1, -1}) == std::vector<int>{1, 1, 0},
          "density ties keep item order");
    // A later, lighter item still fits after a heavier one is skipped.
    KnapsackProblem skip;
    skip.capacity = 5;
    skip.items = {makeItem("big", 4, 40), makeItem("huge", 3, 27), makeItem("small", 1, 1)};
    ScoreCalculator::prepare(skip);
    check(greedySelection(skip, {-1, -1, -1}) == std::vector<int>{1, 0, 1},
          "skips what does not fit, keeps going");

    bool threw = false;
    try {
        greedySelection(p, {-1, -1});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "a fixed list of the wrong length throws");
    threw = false;
    try {
        greedySelection(p, {2, -1, -1});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "a fixed value outside {-1, 0, 1} throws");
    std::printf("\n");
}

Score bruteForceOptimum(KnapsackProblem problem) {
    const int n = problem.itemCount();
    Score best{std::numeric_limits<std::int64_t>::min(), -std::numeric_limits<double>::infinity()};
    for (std::uint32_t mask = 0; mask < (1u << n); ++mask) {
        for (int i = 0; i < n; ++i)
            problem.items[i].selected = (mask >> i) & 1u ? kSelected : kNotSelected;
        const Score score = ScoreCalculator::score(problem);
        if (score > best) best = score;
    }
    return best;
}

// -------------------------------------------------------------------------
// (4) The solver.
// -------------------------------------------------------------------------
void verifySolver() {
    std::printf("== (4) Solver ==\n");
    {
        // ScoreChecks::Full re-checks every incremental score against a reset.
        KnapsackProblem p = randomInstance(30, 77);
        Solver::Config config;
        config.time_limit = std::chrono::milliseconds(1000);
        config.step_limit = 4000;
        config.seed = 3;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruin_recreate_unimproved_step_trigger = 200;  // exercise the -1 ruins too
        bool ok = true;
        try {
            Solver(config).solve(p);
        } catch (const std::exception& error) {
            std::printf("  %s\n", error.what());
            ok = false;
        }
        KnapsackProblem rescored = p;
        check(ok && sameScore(p.score, ScoreCalculator::score(rescored)),
              "FULL_ASSERT solve with ALNS kicks: no score corruption, score = full recompute");
    }
    {
        // Multidimensional 0/1 knapsacks (capacity + resources): the search
        // reaches the brute-force optimum.
        int reached = 0;
        const int instances = 8;
        for (std::uint64_t seed = 11; seed < 11 + instances; ++seed) {
            KnapsackProblem p = randomInstance(8 + static_cast<int>(seed % 6), seed, 0.0, false);
            const Score optimum = bruteForceOptimum(p);
            Solver::Config config;
            config.time_limit = std::chrono::milliseconds(5000);
            config.step_limit = 30000;
            config.seed = seed;
            Solver(config).solve(p);
            if (sameScore(p.score, optimum))
                ++reached;
            else
                std::printf("  seed %llu: %s, optimum %s\n", static_cast<unsigned long long>(seed),
                            p.score.str().c_str(), optimum.str().c_str());
        }
        check(reached == instances,
              "random 0/1 knapsacks with <= 13 items reach the brute-force optimum");
    }
    {
        // Instances with every fact (mandatory / forbidden items, groups,
        // dependencies): the hard landscape can be deceptive (a single flip
        // of a mandatory item may cost more than it repairs), and a rare
        // instance stays in a local optimum (seed 713 of 21..1020 at 20 000
        // steps). These 40 reach the brute-force optimum under their fixed
        // seed and step limit (the time limit is only a backstop), and none
        // ends below the construction.
        int reached = 0;
        bool bounded = true;
        const int instances = 40;
        for (std::uint64_t seed = 21; seed < 21 + instances; ++seed) {
            KnapsackProblem p = randomInstance(8 + static_cast<int>(seed % 6), seed);
            const Score optimum = bruteForceOptimum(p);
            KnapsackProblem start = p;
            Solver::Config zero;
            zero.time_limit = std::chrono::milliseconds(0);
            Solver(zero).solve(start);
            Solver::Config config;
            config.time_limit = std::chrono::milliseconds(30000);
            config.step_limit = 20000;
            config.seed = seed;
            Solver(config).solve(p);
            bounded = bounded && p.score >= start.score && p.score <= optimum;
            if (sameScore(p.score, optimum))
                ++reached;
            else
                std::printf("  seed %llu: %s, optimum %s\n", static_cast<unsigned long long>(seed),
                            p.score.str().c_str(), optimum.str().c_str());
        }
        check(bounded, "instances with every fact: construction <= solved <= brute-force optimum");
        check(reached == instances,
              "random instances with every fact and <= 13 items reach the brute-force optimum");
    }
    {
        // A complete seed with a zero budget comes back unchanged, and the
        // seed is consumed.
        KnapsackProblem p = basicInstance();
        select(p, {0, 1, 2});
        p.warmStart = true;
        Solver::Config config;
        config.time_limit = std::chrono::milliseconds(0);
        Solver(config).solve(p);
        check(p.assignment() == std::vector<int>{1, 1, 1} && p.score.hard == -5 && !p.warmStart,
              "warm start + zero budget: the seed unchanged, warmStart cleared");
        // Without the seed, the zero budget returns the greedy construction.
        select(p, {0, 1, 2});
        Solver(config).solve(p);
        check(p.assignment() == std::vector<int>{0, 1, 1},
              "zero budget without a seed: the greedy {B, C}");
        // A seed with undecided items: the greedy fills them around its load.
        p.items[0].selected = kSelected;
        p.items[1].selected = kUndecided;
        p.items[2].selected = kUndecided;
        p.warmStart = true;
        Solver(config).solve(p);
        check(p.assignment() == std::vector<int>{1, 1, 0},
              "partial seed: A kept, B filled in, C does not fit");
    }
    {
        KnapsackProblem a = randomInstance(60, 5);
        KnapsackProblem b = a;
        Solver::Config config;
        config.time_limit = std::chrono::milliseconds(20000);
        config.step_limit = 3000;
        config.seed = 9;
        Solver(config).solve(a);
        Solver(config).solve(b);
        check(a.assignment() == b.assignment() && sameScore(a.score, b.score),
              "same seed + step limit: the same selection and score");
    }
    {
        KnapsackProblem p = randomInstance(80, 6);
        Solver::Config config;
        config.time_limit = std::chrono::milliseconds(300);
        config.parallel_solver_count = 4;
        config.seed = 1;
        std::int64_t calls = 0;
        config.on_new_best = [&calls](std::int64_t, const Score&) { ++calls; };
        Solver(config).solve(p);
        KnapsackProblem rescored = p;
        check(sameScore(p.score, ScoreCalculator::score(rescored)) && calls > 0,
              "parallel_solver_count 4: a consistent best solution, on_new_best fired");
    }
    {
        KnapsackProblem p;
        p.capacity = 3;
        Solver().solve(p);
        check(p.score.hard == 0 && p.score.soft == 0.0, "no items: solves to 0hard/0soft");
    }
    {
        int thrown = 0;
        auto expectInvalid = [&thrown](KnapsackProblem p) {
            try {
                ScoreCalculator::prepare(p);
            } catch (const std::invalid_argument&) {
                ++thrown;
            }
        };
        KnapsackProblem bad = basicInstance();
        bad.items[0].weight = 0;
        expectInvalid(bad);
        bad = basicInstance();
        bad.mandatoryItems = {0};
        bad.forbiddenItems = {0};
        expectInvalid(bad);
        bad = basicInstance();
        bad.mandatoryItems = {3};
        expectInvalid(bad);
        bad = basicInstance();
        bad.items[1].requiredItems = {-1};
        expectInvalid(bad);
        bad = basicInstance();
        bad.selectionGroups.push_back({"g", {0, 0}, 0, std::nullopt});
        expectInvalid(bad);
        bad = basicInstance();
        bad.selectionGroups.push_back({"g", {0, 1}, 2, 1});
        expectInvalid(bad);
        bad = basicInstance();
        bad.resources = {{"r", 1}, {"r", 2}};
        expectInvalid(bad);
        bad = basicInstance();
        bad.items[2].selected = 2;
        expectInvalid(bad);
        bad = basicInstance();
        bad.items[2].value = -1;
        expectInvalid(bad);
        bad = basicInstance();
        bad.items[2].id = "A";
        expectInvalid(bad);
        check(thrown == 10, "prepare() refuses each invalid fact with std::invalid_argument");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (5) CoreRepackMove.
// -------------------------------------------------------------------------
// A plain knapsack in the shape of benchmarks/knapsack: weights 5..60, values
// 10..100, capacity a third of the total weight.
KnapsackProblem plainKnapsack(int itemCount, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    KnapsackProblem problem;
    std::int64_t totalWeight = 0;
    for (int i = 0; i < itemCount; ++i) {
        const int weight = 5 + static_cast<int>(rng() % 56);
        const int value = 10 + static_cast<int>(rng() % 91);
        problem.items.push_back(makeItem("I" + std::to_string(i), weight, value));
        totalWeight += weight;
    }
    problem.capacity = totalWeight / 3;
    return problem;
}

// The optimum of a plain knapsack: the best value per load, item by item.
std::int64_t dynamicProgrammingOptimum(const KnapsackProblem& problem) {
    std::vector<std::int64_t> best(static_cast<std::size_t>(problem.capacity) + 1, 0);
    for (const Item& item : problem.items) {
        for (std::int64_t load = problem.capacity; load >= item.weight; --load) {
            best[load] = std::max(best[load], best[load - item.weight] + item.value);
        }
    }
    return best[problem.capacity];
}

void verifyCoreRepackMove() {
    std::printf("== (5) CoreRepackMove ==\n");
    {
        // Through an Editor, as the engine drives it, from random feasible
        // selections of plain knapsacks: a move raises the value, stays
        // within the capacity, reports the items it flipped, leaves the
        // incremental score equal to a full recompute, and its undo restores
        // the selection and the score. A selection over capacity is declined.
        int moves = 0, notBetter = 0, overCapacity = 0, badReport = 0, mismatches = 0;
        int undoMismatches = 0;
        for (std::uint64_t seed = 1; seed <= 4; ++seed) {
            KnapsackProblem p = plainKnapsack(60 + static_cast<int>(seed) * 80, seed);
            ScoreCalculator::prepare(p);
            constructSelection(p);
            std::mt19937_64 rng(seed * 7);
            // Loosen the greedy selection so the move has work to do.
            for (Item& item : p.items) {
                if (item.isSelected() && rng() % 4 == 0) item.selected = kNotSelected;
            }
            KnapsackIncrementalScoreCalculator calculator;
            optimaru::Editor<KnapsackProblem> editor(p, calculator);
            optimaru::Neighborhoods nearby;
            const std::vector<int> noLists;
            optimaru::OperatorContext<KnapsackProblem> ctx{editor, nearby, noLists, noLists};
            CoreRepackMove move(p);
            std::vector<int> entities;
            for (int step = 0; step < 200; ++step) {
                const std::vector<int> before = p.assignment();
                const Score scoreBefore = editor.score();
                const std::int64_t valueBefore = p.totalValue();
                entities.clear();
                if (!move.apply(ctx, rng, entities)) continue;
                ++moves;
                if (p.totalValue() <= valueBefore) ++notBetter;
                if (p.totalWeight() > p.capacity) ++overCapacity;
                std::vector<int> changed;
                for (int i = 0; i < p.itemCount(); ++i) {
                    if (p.items[i].selected != before[i]) changed.push_back(i);
                }
                std::vector<int> reported = entities;
                std::sort(reported.begin(), reported.end());
                if (changed.empty() || changed != reported) ++badReport;
                KnapsackProblem rescored = p;
                if (!sameScore(editor.score(), ScoreCalculator::score(rescored))) ++mismatches;
                if (step % 2 == 0) {  // keep every other move, undo the rest
                    move.undo_last(ctx);
                    if (p.assignment() != before || !sameScore(editor.score(), scoreBefore)) {
                        ++undoMismatches;
                    }
                }
            }
        }
        check(moves > 0 && notBetter == 0 && overCapacity == 0,
              std::to_string(moves) + " moves: each raises the value within the capacity");
        check(badReport == 0, "a move reports exactly the items it flipped");
        check(mismatches == 0, "incremental == full recompute after a move");
        check(undoMismatches == 0, "undo_last restores the selection and the score");

        KnapsackProblem over = plainKnapsack(40, 9);
        ScoreCalculator::prepare(over);
        for (Item& item : over.items) item.selected = kSelected;
        KnapsackIncrementalScoreCalculator calculator;
        optimaru::Editor<KnapsackProblem> editor(over, calculator);
        optimaru::Neighborhoods nearby;
        const std::vector<int> noLists;
        optimaru::OperatorContext<KnapsackProblem> ctx{editor, nearby, noLists, noLists};
        CoreRepackMove move(over);
        std::mt19937_64 rng(1);
        std::vector<int> entities;
        check(!move.apply(ctx, rng, entities) && entities.empty(),
              "a selection over capacity is declined");
    }
    {
        // Plain knapsacks of 300 to 2,000 items under a step limit: with the
        // move the solve ends on the dynamic-programming optimum (the
        // built-in moves alone stayed below it on three when this was
        // written).
        int reached = 0, reachedWithout = 0;
        const int instances = 6;
        for (int index = 0; index < instances; ++index) {
            const int itemCount = 300 + 340 * index;
            const std::uint64_t seed = 40 + static_cast<std::uint64_t>(index);
            const std::int64_t optimum = dynamicProgrammingOptimum(plainKnapsack(itemCount, seed));
            for (bool coreRepacks : {true, false}) {
                KnapsackProblem p = plainKnapsack(itemCount, seed);
                Solver::Config config;
                config.time_limit = std::chrono::minutes(10);  // the step limit binds
                config.step_limit = 400000;
                config.seed = seed;
                config.core_repack_moves = coreRepacks;
                Solver(config).solve(p);
                const bool optimal = p.score.hard == 0 && p.totalValue() == optimum;
                (coreRepacks ? reached : reachedWithout) += optimal;
                if (coreRepacks && !optimal) {
                    std::printf("  %d items, seed %llu: %lld, optimum %lld\n", itemCount,
                                static_cast<unsigned long long>(seed),
                                static_cast<long long>(p.totalValue()),
                                static_cast<long long>(optimum));
                }
            }
        }
        check(reached == instances, "plain knapsacks reach the dynamic-programming optimum");
        // For the record, not a check: the built-in moves may get there too one day.
        std::printf("    without the move %d of %d reach it\n", reachedWithout, instances);
    }
    std::printf("\n");
}

}  // namespace

int main() {
    verifyHandCalculated();
    verifyIncrementalWalk();
    verifyGreedy();
    verifySolver();
    verifyCoreRepackMove();
    if (failures) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}

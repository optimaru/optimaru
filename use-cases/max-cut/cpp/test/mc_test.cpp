// Monolithic-path tests, engine test/ style: check() + a non-zero exit on any
// failure.
//   (1) hand-calculated raw costs and scores, each on the full recompute AND
//       the incremental calculator: a positive and a zero case per constraint,
//       plus the edge cases (self-loops, parallel and zero-weight edges,
//       self-pairs, the -1 rules)
//   (2) incremental == full recompute across random flips, swaps and -1
//       (unassigned) transitions on random instances using every fact: the
//       score and every constraint's raw total after each move, each
//       constraint non-zero at some point of the walk
//   (3) the solver: FULL_ASSERT (ALNS kicks included), the exact-size
//       contract, pinning under multi-start, a worker exception rethrown, the
//       zero budget, an empty graph, brute-force optima, reproducibility
//       under a step limit, validation
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "mc/domain.hpp"
#include "mc/incremental_calculator.hpp"
#include "mc/score_calculator.hpp"
#include "mc/solver.hpp"
#include "mc_fixtures.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace mc;
using namespace mc_test;

namespace
{

// The full recompute and a freshly reset incremental calculator on the
// current partition; checks that they agree and returns the full one.
Score scoreBothWays(MaxCutProblem &problem, const std::string &label)
{
    const Score full = ScoreCalculator::calculateScore(problem);
    MaxCutIncrementalScoreCalculator incremental;
    incremental.resetWorkingSolution(problem);
    const Score fast = incremental.calculateScore();
    if (!sameScore(full, fast))
        check(false, label + ": incremental " + fast.toString() + " != full " + full.toString());
    return full;
}

void expectScore(MaxCutProblem &problem, std::int64_t hard, double soft, const std::string &label)
{
    const Score score = scoreBothWays(problem, label);
    check(score.hard == hard && score.soft == soft, label + " -> " + score.toString());
}

bool pairIs(const std::int64_t (&costs)[kSideCount], std::int64_t a, std::int64_t b)
{
    return costs[0] == a && costs[1] == b;
}

// -------------------------------------------------------------------------
// (1) Hand-calculated raw costs and scores.
// -------------------------------------------------------------------------
void verifyHandCalculated()
{
    std::printf("== (1) Hand-calculated costs, positive and zero per constraint ==\n");
    {
        MaxCutProblem p = fixtureH();
        p.side = {1, 1, 0, 0};
        ScoreCalculator::prepare(p);
        const RawCosts raw = ScoreCalculator::rawCosts(p);
        check(pairIs(raw.fixed, 0, 0), "fixedPartitionCost zero: A on its fixed side 1");
        check(pairIs(raw.same, 0, 0), "samePartitionPairsCost zero: A and B both on side 1");
        check(pairIs(raw.different, 0, 0), "differentPartitionPairsCost zero: B on 1, C on 0");
        check(pairIs(raw.size, 0, 0), "partitionSizeBoundsCost zero: 2 + 2 within [2, 2]");
        check(raw.imbalance == 0, "partitionImbalanceCost zero: equal sides");
        check(raw.cut == 3, "cutWeightCost: only B-C (3) is cut");
        expectScore(p, 0, 3.0, "fixture H zero case");
    }
    {
        MaxCutProblem p = fixtureH();
        p.side = {0, 1, 1, 1};
        ScoreCalculator::prepare(p);
        const RawCosts raw = ScoreCalculator::rawCosts(p);
        check(pairIs(raw.fixed, 1, 0), "fixedPartitionCost positive: A (fixed 1) on side 0 -> [1, 0]");
        check(pairIs(raw.same, 1, 0), "samePartitionPairsCost positive: (A, B) split, charged to A's side 0");
        check(pairIs(raw.different, 0, 1), "differentPartitionPairsCost positive: B, C both on side 1");
        check(pairIs(raw.size, 1, 1), "partitionSizeBoundsCost positive: side 0 one short, side 1 one over");
        check(raw.imbalance == 2, "partitionImbalanceCost positive: |1 - 3| = 2");
        check(raw.cut == 2, "cutWeightCost: only A-B (2) is cut");
        expectScore(p, -5, 2.0, "fixture H positive case");
    }
    {
        MaxCutProblem p = triangle();
        p.side = {0, 1, 0};
        expectScore(p, 0, 5.0, "triangle 0,1,0 cuts A-B and B-C: 5");
        p.side = {0, 0, 1};
        expectScore(p, 0, 7.0, "triangle 0,0,1 cuts B-C and A-C: 7 (the optimum)");
        p.side = {1, 1, 1};
        expectScore(p, 0, 0.0, "nothing cut: soft 0");
        check(!std::signbit(p.score.soft), "an empty cut scores +0.0, never -0.0");
    }
    {
        MaxCutProblem p = contractInstance();
        ScoreCalculator::prepare(p);
        const RawCosts raw = ScoreCalculator::rawCosts(p);
        check(raw.fixedTotal() == 1 && raw.sameTotal() == 1 && raw.differentTotal() == 1 && raw.sizeTotal() == 0
                  && raw.imbalance == 1 && raw.cut == 0,
              "the 0.1.x contract instance: fixed / same / different 1 each, size 0, imbalance 1, cut 0");
        expectScore(p, -3, 0.0, "contract instance score");
    }
    {
        // A self-loop is never cut; parallel edges add up; zero weights count 0.
        MaxCutProblem p = graph({"A", "B"}, {{0, 0, 5}, {0, 1, 1}, {1, 0, 2}, {0, 1, 0}});
        p.side = {0, 1};
        expectScore(p, 0, 3.0, "self-loop 5 + parallel A-B 1, 2 and a zero-weight edge: cut 3");
        p.side = {1, 1};
        expectScore(p, 0, 0.0, "the same graph on one side: cut 0");
    }
    {
        // Self-pairs: a same self-pair is never split, a different self-pair
        // is always joined; repeated pairs count once per entry.
        MaxCutProblem p = graph({"A", "B"}, {});
        p.samePartitionPairs = {{0, 0}, {0, 1}, {0, 1}};
        p.differentPartitionPairs = {{1, 1}, {0, 1}};
        p.side = {0, 1};
        ScoreCalculator::prepare(p);
        RawCosts raw = ScoreCalculator::rawCosts(p);
        check(pairIs(raw.same, 2, 0) && pairIs(raw.different, 0, 1),
              "same (A,A) 0 + (A,B) twice -> [2, 0]; different (B,B) -> [0, 1], (A,B) split");
        expectScore(p, -3, 0.0, "self-pair score");
        p.side = {1, 1};
        ScoreCalculator::prepare(p);
        raw = ScoreCalculator::rawCosts(p);
        check(pairIs(raw.same, 0, 0) && pairIs(raw.different, 0, 2),
              "both on side 1: same 0, different (B,B) + (A,B) -> [0, 2]");
    }
    {
        // Size bounds with only one bound given.
        MaxCutProblem p = graph({"A", "B", "C", "D", "E"}, {});
        p.maxPartitionSize = 3;
        p.side = {0, 0, 0, 0, 1};
        ScoreCalculator::prepare(p);
        check(pairIs(ScoreCalculator::rawCosts(p).size, 1, 0), "max 3 only: side 0 holds 4 -> [1, 0]");
        p.maxPartitionSize.reset();
        p.minPartitionSize = 2;
        ScoreCalculator::prepare(p);
        check(pairIs(ScoreCalculator::rawCosts(p).size, 0, 1), "min 2 only: side 1 holds 1 -> [0, 1]");
        p.side = {0, 0, 1, 1, 1};
        ScoreCalculator::prepare(p);
        check(pairIs(ScoreCalculator::rawCosts(p).size, 0, 0), "min 2 only: 2 + 3 -> [0, 0]");
        expectScore(p, 0, 0.0, "within the bounds");
    }
    {
        // The -1 rules (mc/domain.hpp): an unassigned vertex is in no side.
        MaxCutProblem p = graph({"A", "B", "C"}, {{0, 1, 4}, {1, 2, 5}});
        p.fixedSide[2] = 1;
        p.samePartitionPairs = {{0, 1}, {1, 0}};
        p.differentPartitionPairs = {{0, 2}, {1, 2}};
        p.minPartitionSize = 1;
        p.side = {0, kUnassigned, kUnassigned};
        ScoreCalculator::prepare(p);
        const RawCosts raw = ScoreCalculator::rawCosts(p);
        check(raw.cut == 0, "-1: an edge with an unassigned endpoint is not cut");
        check(pairIs(raw.same, 1, 0),
              "-1: (A, B) is split (A assigned, B not); (B, A) charges nothing (B unassigned)");
        check(pairIs(raw.different, 0, 0), "-1: a pair with an unassigned endpoint is never joined");
        check(pairIs(raw.fixed, 0, 0), "-1: an unassigned fixed vertex is not off its side");
        check(pairIs(raw.size, 0, 1) && raw.imbalance == 1, "-1: the sizes count assigned vertices only");
        expectScore(p, -2, 0.0, "-1 score, incremental == full");
    }
    {
        MaxCutProblem p = graph({}, {});
        expectScore(p, 0, 0.0, "the empty graph scores 0hard/0soft");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (2) Incremental == full recompute on random walks, per constraint.
// -------------------------------------------------------------------------
bool sameTotals(const MaxCutIncrementalScoreCalculator::Totals &totals, const RawCosts &raw)
{
    return totals.fixed == raw.fixedTotal() && totals.same == raw.sameTotal()
           && totals.different == raw.differentTotal() && totals.size[0] == raw.size[0]
           && totals.size[1] == raw.size[1] && totals.imbalance == raw.imbalance && totals.cut == raw.cut;
}

void verifyIncrementalWalk()
{
    std::printf("== (2) Incremental == full recompute on random walks, per constraint ==\n");
    // The catalog order; nonZero counts the steps on which each raw cost was
    // non-zero, so a constraint the walk never exercises fails the test.
    const char *const names[] = {"fixedPartitionCost",      "samePartitionPairsCost", "differentPartitionPairsCost",
                                 "partitionSizeBoundsCost", "partitionImbalanceCost", "cutWeightCost"};
    int nonZero[6] = {0, 0, 0, 0, 0, 0};
    int mismatches = 0;
    int costMismatches = 0;
    int steps = 0;
    for (std::uint64_t seed = 1; seed <= 12; ++seed)
    {
        MaxCutProblem p = randomInstance(1 + static_cast<int>((seed * 7) % 40), seed, 0.1);
        ScoreCalculator::prepare(p);
        MaxCutIncrementalScoreCalculator calculator;
        optimaru::ScoreDirector<MaxCutProblem> director(p, calculator);
        MaxCutProblem shadow = p;
        std::mt19937_64 rng(seed * 101);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const int n = p.vertexCount();
        for (int step = 0; step < 250; ++step, ++steps)
        {
            const double u = unit(rng);
            if (u < 0.6)
            {
                const int v = static_cast<int>(rng() % n);
                const int value = unit(rng) < 0.15 ? kUnassigned : static_cast<int>(rng() % 2);
                director.changeVariable(v, value);
            }
            else
            {
                // A swap (or a random 3-vertex batch) as one composite change.
                director.beginVariableChanges();
                const int a = static_cast<int>(rng() % n);
                const int b = static_cast<int>(rng() % n);
                const int va = p.side[a];
                director.changeVariable(a, p.side[b]);
                director.changeVariable(b, va);
                if (u > 0.9)
                    director.changeVariable(static_cast<int>(rng() % n), static_cast<int>(rng() % 2));
                director.endVariableChanges();
            }
            shadow.side = p.side;
            const Score full = ScoreCalculator::calculateScore(shadow);
            const RawCosts raw = ScoreCalculator::rawCosts(shadow);
            const Score incremental = director.calculateScore();
            if (!sameScore(full, incremental) && ++mismatches <= 3)
            {
                std::printf("  MISMATCH seed %llu step %d: incremental=%s full=%s\n",
                            static_cast<unsigned long long>(seed), step, incremental.toString().c_str(),
                            full.toString().c_str());
            }
            // Per constraint, so opposite errors in two terms cannot cancel
            // out and the weight-0 imbalance is checked too.
            if (!sameTotals(calculator.totals(), raw) && ++costMismatches <= 3)
            {
                std::printf("  RAW COST MISMATCH seed %llu step %d\n", static_cast<unsigned long long>(seed), step);
            }
            const std::int64_t costs[6] = {raw.fixedTotal(), raw.sameTotal(), raw.differentTotal(),
                                           raw.sizeTotal(),  raw.imbalance,   raw.cut};
            for (int c = 0; c < 6; ++c)
                nonZero[c] += costs[c] != 0 ? 1 : 0;
        }
    }
    check(mismatches == 0, std::to_string(steps) + " random moves, incremental == full recompute");
    check(costMismatches == 0,
          std::to_string(steps) + " random moves, every constraint's incremental raw total == the full recompute's");
    for (int c = 0; c < 6; ++c)
    {
        check(nonZero[c] > 0,
              std::string(names[c]) + " non-zero on " + std::to_string(nonZero[c]) + "/" + std::to_string(steps)
                  + " steps of the walk");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (3) The solver.
// -------------------------------------------------------------------------
Solver::Config steps(std::int64_t count, std::uint64_t seed)
{
    Solver::Config config;
    config.spentLimit = std::chrono::seconds(30);
    config.stepCountLimit = count;
    config.randomSeed = seed;
    return config;
}

bool solvesCleanly(MaxCutProblem &p, const Solver::Config &config)
{
    try
    {
        Solver(config).solve(p);
    }
    catch (const std::exception &error)
    {
        std::printf("  %s\n", error.what());
        return false;
    }
    MaxCutProblem rescored = p;
    return sameScore(p.score, ScoreCalculator::calculateScore(rescored));
}

void verifySolver()
{
    std::printf("== (3) Solver ==\n");
    {
        // FULL_ASSERT re-checks every incremental score against a reset; the
        // early ALNS trigger exercises the -1 ruins too.
        MaxCutProblem p = randomInstance(30, 77, 0.0, /*bounds=*/false);
        Solver::Config config = steps(4000, 3);
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        config.ruinRecreateUnimprovedStepTrigger = 200;
        config.ruinRecreatePolishSteps = 200;
        config.ruinRecreateAsMoveWeight = 0.05;
        check(solvesCleanly(p, config), "FULL_ASSERT solve with ALNS kicks: no score corruption");
    }
    {
        // Exact size: the solver forces swaps only, whatever the config says.
        MaxCutProblem p = randomInstance(30, 78, 0.0, /*bounds=*/false);
        p.minPartitionSize = p.maxPartitionSize = 15;
        Solver::Config config = steps(4000, 4);
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        config.changeMoveWeight = 1.0;
        config.ruinRecreateUnimprovedStepTrigger = 50;
        check(solvesCleanly(p, config), "FULL_ASSERT exact-size solve: no score corruption");
    }
    {
        // Exact size: a seed of the right size and one of the wrong size keep
        // their sizes (the 0.1.x contract), the wrong one its hard penalty.
        MaxCutProblem right = graph({"A", "B", "C", "D"}, {{0, 1, 2}, {1, 2, 3}, {2, 3, 4}});
        right.minPartitionSize = right.maxPartitionSize = 2;
        MaxCutProblem wrong = right;
        right.side = {1, 1, 0, 0};
        right.warmStart = true;
        wrong.side = {1, 1, 1, 0};
        wrong.warmStart = true;
        Solver::Config config = steps(500, 1);
        config.changeMoveWeight = 1.0;
        config.swapMoveWeight = 0.0;
        config.ruinRecreateUnimprovedStepTrigger = 10;
        Solver(config).solve(right);
        Solver(config).solve(wrong);
        check(right.sideSize(1) == 2 && right.score.hard == 0, "a right-size seed stays at 2 + 2");
        check(right.score.soft == 9.0, "... and reaches the best balanced cut (A, C | B, D cuts all three edges: 9)");
        check(wrong.sideSize(1) == 3 && wrong.score.hard == -2, "a wrong-size seed keeps 3 + 1 and its -2 hard");
    }
    {
        // Pinned vertices never move, under multi-start too.
        bool kept = true;
        for (std::uint64_t seed = 0; seed < 5; ++seed)
        {
            MaxCutProblem p = graph({"A", "B", "C", "D"}, {{0, 1, 2}, {1, 2, 3}, {2, 3, 4}});
            p.fixedSide = {1, kUnassigned, 1, kUnassigned};
            p.minPartitionSize = p.maxPartitionSize = 2;
            Solver::Config config = steps(200, seed);
            config.parallelSolverCount = 3;
            Solver(config).solve(p);
            kept = kept && p.side[0] == 1 && p.side[2] == 1;
        }
        check(kept, "fixed vertices A, C stay on side 1 (5 seeds, 3 parallel starts)");
        MaxCutProblem free = randomInstance(20, 5, 0.0, false, /*offFixed=*/false);
        const std::vector<int> fixedSide = free.fixedSide;
        Solver::Config config = steps(3000, 9);
        config.parallelSolverCount = 3;
        config.ruinRecreateUnimprovedStepTrigger = 100;
        Solver(config).solve(free);
        bool pinned = true;
        for (int v = 0; v < free.vertexCount(); ++v)
            pinned = pinned && (fixedSide[v] < 0 || free.side[v] == fixedSide[v]);
        check(pinned, "random instance: no fixed vertex moved (ALNS kicks, 3 starts)");
    }
    {
        // An exception in a parallel worker (here a throwing onNewBest on
        // worker 0) is rethrown by solve() once every worker has joined,
        // instead of terminating the process; it also stops the other
        // workers, which would otherwise run the whole 30 s budget.
        MaxCutProblem p = randomInstance(40, 31, 0.0, /*bounds=*/false);
        Solver::Config config;
        config.parallelSolverCount = 3;
        config.onNewBest = [](std::int64_t, const Score &) { throw std::runtime_error("onNewBest failed"); };
        const auto start = std::chrono::steady_clock::now();
        bool rethrown = false;
        try
        {
            Solver(config).solve(p);
        }
        catch (const std::runtime_error &error)
        {
            rethrown = std::string(error.what()) == "onNewBest failed";
        }
        check(rethrown, "a throwing onNewBest under 3 parallel starts is rethrown by solve()");
        check(std::chrono::steady_clock::now() - start < std::chrono::seconds(10),
              "... and stops the other workers early (budget 30 s)");
    }
    {
        // The zero budget returns the start partition, scored.
        Solver::Config none;
        none.spentLimit = std::chrono::milliseconds(0);
        MaxCutProblem p = graph({"A", "B", "C", "D"}, {{0, 1, 2}, {1, 2, 3}, {2, 3, 4}});
        p.side = {1, 1, 1, 1};  // ignored: no warm start
        Solver(none).solve(p);
        check(p.side == std::vector<int>({0, 1, 0, 1}) && p.scored && p.score.soft == 9.0,
              "no warm start: the alternating start, scored (cut 9)");
        p.fixedSide = {kUnassigned, kUnassigned, 1, kUnassigned};
        Solver(none).solve(p);
        check(p.side == std::vector<int>({0, 1, 1, 1}), "a fixed vertex starts on its fixed side");
        p.side = {1, kUnassigned, kUnassigned, 0};
        p.warmStart = true;
        Solver(none).solve(p);
        check(p.side == std::vector<int>({1, 1, 1, 0}) && !p.warmStart,
              "a partial warm start keeps its part, fills the rest, and is consumed");
        p.side = {1, 1, 0, 0};  // contradicts C's fixed side 1
        p.warmStart = true;
        Solver(none).solve(p);
        check(p.side == std::vector<int>({1, 1, 0, 0}) && p.score.hard == -1,
              "a seed against a fixed side is kept, with its hard penalty");
    }
    {
        MaxCutProblem empty = graph({}, {});
        Solver(steps(100, 1)).solve(empty);
        check(empty.scored && sameScore(empty.score, Score{0, 0.0}), "the empty graph is scored, not searched");
        MaxCutProblem single = graph({"A"}, {});
        single.minPartitionSize = single.maxPartitionSize = 0;
        Solver(steps(100, 1)).solve(single);
        check(single.side == std::vector<int>({0}) && single.score.hard == -1,
              "exact size with one movable vertex: no search (no size-changing fallback)");
    }
    {
        // Brute-force optima on small random instances.
        int reached = 0;
        const int instances = 20;
        for (std::uint64_t seed = 11; seed < 11 + instances; ++seed)
        {
            MaxCutProblem p = randomInstance(6 + static_cast<int>(seed % 7), seed, 0.0, /*bounds=*/seed % 2 == 0);
            if (p.exactSize())
                p.maxPartitionSize.reset();
            const Score optimum = bruteForceOptimum(p);
            Solver::Config config = steps(20000, seed);
            config.spentLimit = std::chrono::seconds(5);
            config.bestScoreLimit = optimum;
            // Same-side pairs chain vertices into clusters that only move
            // together; a single flip of a cluster member always loses hard
            // first, so plain late acceptance can stall on such an instance.
            // The ruin-and-recreate as-move unassigns a few vertices at once
            // and puts them back greedily, which moves a whole cluster.
            config.ruinRecreateAsMoveWeight = 0.05;
            Solver(config).solve(p);
            reached += sameScore(p.score, optimum) ? 1 : 0;
        }
        check(reached == instances, std::to_string(reached) + "/" + std::to_string(instances)
                                        + " small instances reach the brute-force optimum");
    }
    {
        // Reproducible under a step limit; multi-start never scores below one
        // start.
        MaxCutProblem a = randomInstance(40, 21, 0.0, false);
        MaxCutProblem b = a;
        Solver(steps(3000, 7)).solve(a);
        Solver(steps(3000, 7)).solve(b);
        check(a.side == b.side && sameScore(a.score, b.score), "same seed and step limit: same partition");
        MaxCutProblem multi = randomInstance(40, 21, 0.0, false);
        Solver::Config config = steps(3000, 7);
        config.parallelSolverCount = 3;
        Solver(config).solve(multi);
        check(a.score <= multi.score, "3 parallel starts (seed 7 included) score >= the single start");
    }
    {
        // Validation.
        auto rejects = [](MaxCutProblem p)
        {
            try
            {
                ScoreCalculator::prepare(p);
            }
            catch (const std::invalid_argument &)
            {
                return true;
            }
            return false;
        };
        MaxCutProblem base = graph({"A", "B"}, {{0, 1, 1}});
        MaxCutProblem p = base;
        p.edges.push_back({0, 2, 1});
        check(rejects(p), "an edge to an unknown vertex index is refused");
        p = base;
        p.edges[0].weight = -1;
        check(rejects(p), "a negative edge weight is refused");
        p = base;
        p.vertexIds = {"A", "A"};
        check(rejects(p), "a duplicate vertex id is refused");
        p = base;
        p.side = {0, 2};
        check(rejects(p), "a side outside {-1, 0, 1} is refused");
        p = base;
        p.side = {0};
        check(rejects(p), "a side vector of the wrong length is refused");
        p = base;
        p.fixedSide = {3, -1};
        check(rejects(p), "a fixed side outside {-1, 0, 1} is refused");
        p = base;
        p.samePartitionPairs = {{0, 5}};
        check(rejects(p), "a pair naming an unknown vertex is refused");
        p = base;
        p.minPartitionSize = 2;
        p.maxPartitionSize = 1;
        check(rejects(p), "min > max is refused");
        p = base;
        p.maxPartitionSize = 3;
        check(rejects(p), "max > |V| is refused");
        p = base;
        p.side.clear();
        p.fixedSide.clear();
        check(!rejects(p), "empty side / fixedSide vectors mean all -1");
        Solver::Config bad;
        bad.ruinRecreateRewards = {1.0};
        bool threw = false;
        try
        {
            Solver(bad).solve(base);
        }
        catch (const std::invalid_argument &)
        {
            threw = true;
        }
        check(threw, "ruinRecreateRewards must hold 4 values");
    }
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyHandCalculated();
    verifyIncrementalWalk();
    verifySolver();
    if (failures)
    {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}

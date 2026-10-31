// Shared test fixtures for mc_test and mc_plugin_test: the check() harness,
// the hand-calculated instances and a seeded random instance generator.
#ifndef MC_TEST_FIXTURES_HPP
#define MC_TEST_FIXTURES_HPP

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "mc/domain.hpp"
#include "mc/score_calculator.hpp"

namespace mc_test
{

using namespace mc;

inline int failures = 0;

inline void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

inline bool sameScore(const Score &a, const Score &b)
{
    return a.hard == b.hard && a.soft == b.soft;
}

inline MaxCutProblem graph(std::vector<std::string> ids, std::vector<Edge> edges)
{
    MaxCutProblem problem;
    problem.vertexIds = std::move(ids);
    problem.edges = std::move(edges);
    problem.fixedSide.assign(problem.vertexIds.size(), kUnassigned);
    problem.side.assign(problem.vertexIds.size(), kUnassigned);
    return problem;
}

// Fixture H (the port design's hand-calculated instance): A-B 2, B-C 3,
// C-D 4; A fixed to side 1; same pair (A, B); different pair (B, C); exactly
// 2 vertices per side.
//   zero     [1, 1, 0, 0]: every hard cost 0, imbalance 0, cut 3  -> 0hard/3soft
//   positive [0, 1, 1, 1]: fixed [1, 0], same [1, 0], different [0, 1],
//            size [1, 1], imbalance 2, cut 2                     -> -5hard/2soft
inline MaxCutProblem fixtureH()
{
    MaxCutProblem problem = graph({"A", "B", "C", "D"}, {{0, 1, 2}, {1, 2, 3}, {2, 3, 4}});
    problem.fixedSide[0] = 1;
    problem.samePartitionPairs = {{0, 1}};
    problem.differentPartitionPairs = {{1, 2}};
    problem.minPartitionSize = 2;
    problem.maxPartitionSize = 2;
    return problem;
}

// The 0.1.x triangle: A-B 2, B-C 3, A-C 4. Sides 0, 1, 0 cut 5; the optimum
// is 7 (C alone).
inline MaxCutProblem triangle()
{
    return graph({"A", "B", "C"}, {{0, 1, 2}, {1, 2, 3}, {0, 2, 4}});
}

// The 0.1.x contract instance (test_max_cut.py): no edges, A0 B1 C1, A fixed
// to 1, same (A, B), different (B, C), bounds 1..2. Every pair / fixed cost
// sums to 1, the size bounds to 0, the imbalance is 1 and the cut 0.
inline MaxCutProblem contractInstance()
{
    MaxCutProblem problem = graph({"A", "B", "C"}, {});
    problem.fixedSide[0] = 1;
    problem.samePartitionPairs = {{0, 1}};
    problem.differentPartitionPairs = {{1, 2}};
    problem.minPartitionSize = 1;
    problem.maxPartitionSize = 2;
    problem.side = {0, 1, 1};
    return problem;
}

// A seeded random instance using every fact: weighted edges (zero weights,
// self-loops and parallel edges included), fixed vertices, same / different
// pairs (self-pairs and repeats included) and size bounds. The sides are
// random 0 / 1 (fixed vertices on their side unless `offFixed`), -1 with
// probability `unassigned`.
inline MaxCutProblem randomInstance(int n,
                                    std::uint64_t seed,
                                    double unassigned = 0.0,
                                    bool bounds = true,
                                    bool offFixed = true)
{
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    auto pick = [&](int count) { return static_cast<int>(rng() % static_cast<std::uint64_t>(count)); };
    std::vector<std::string> ids;
    for (int v = 0; v < n; ++v)
        ids.push_back("V" + std::to_string(v));
    std::vector<Edge> edges;
    if (n > 0)
    {
        const double density = 0.15 + 0.5 * unit(rng);
        for (int a = 0; a < n; ++a)
        {
            for (int b = a + 1; b < n; ++b)
            {
                if (unit(rng) < density)
                    edges.push_back({unit(rng) < 0.5 ? a : b, unit(rng) < 0.5 ? b : a,
                                     static_cast<std::int64_t>(pick(11))});
            }
        }
        for (int extra = pick(3); extra > 0; --extra)
            edges.push_back({pick(n), pick(n), static_cast<std::int64_t>(1 + pick(9))});  // self-loops, parallels
    }
    MaxCutProblem problem = graph(ids, edges);
    if (n == 0)
        return problem;
    for (int v = 0; v < n; ++v)
    {
        if (unit(rng) < 0.15)
            problem.fixedSide[v] = pick(2);
    }
    for (int k = pick(n + 1); k > 0; --k)
        problem.samePartitionPairs.push_back({pick(n), pick(n)});
    for (int k = pick(n + 1); k > 0; --k)
        problem.differentPartitionPairs.push_back({pick(n), pick(n)});
    if (bounds && unit(rng) < 0.7)
    {
        const int lo = pick(n / 2 + 1);
        const int hi = lo + pick(n - lo + 1);
        if (unit(rng) < 0.8)
            problem.minPartitionSize = lo;
        if (unit(rng) < 0.8)
            problem.maxPartitionSize = hi;
    }
    for (int v = 0; v < n; ++v)
    {
        if (unit(rng) < unassigned)
            problem.side[v] = kUnassigned;
        else if (!offFixed && problem.fixedSide[v] >= 0)
            problem.side[v] = problem.fixedSide[v];
        else
            problem.side[v] = pick(2);
    }
    return problem;
}

// The best default score over every complete partition that keeps the
// pinned vertices on their fixed side (n <= ~16).
inline Score bruteForceOptimum(MaxCutProblem problem)
{
    const int n = problem.vertexCount();
    Score best{std::numeric_limits<std::int64_t>::min(), -std::numeric_limits<double>::infinity()};
    for (std::uint64_t mask = 0; mask < (std::uint64_t{1} << n); ++mask)
    {
        bool keeps = true;
        for (int v = 0; v < n; ++v)
        {
            problem.side[v] = static_cast<int>((mask >> v) & 1U);
            if (problem.fixedSide[v] >= 0 && problem.side[v] != problem.fixedSide[v])
                keeps = false;
        }
        if (!keeps)
            continue;
        const Score score = ScoreCalculator::calculateScore(problem);
        if (best < score)
            best = score;
    }
    return best;
}

}  // namespace mc_test

#endif  // MC_TEST_FIXTURES_HPP

#ifndef ALB_TEST_FIXTURES_HPP
#define ALB_TEST_FIXTURES_HPP

// Shared instances of alb_test.cpp and alb_plugin_test.cpp.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "alb/domain.hpp"
#include "alb/score_calculator.hpp"

namespace alb_test
{

using namespace alb;

inline int failures = 0;

inline void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

inline bool sameScore(const Score &a, const Score &b)
{
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

// The ten registry names, in catalog order.
inline const std::vector<std::string> kNames = {
    "stationCapacityCost",       "taskPrecedenceCost",    "maxStationsCost",
    "taskStationBoundsCost",     "fixedTaskStationsCost", "forbiddenTaskStationsCost",
    "sameStationGroupsCost",     "differentStationPairsCost", "usedStationsCost",
    "workloadImbalanceCost",
};

// The ten raw totals by registry name (the keys of constraint_costs()).
inline std::map<std::string, double> namedTotals(const RawCosts &raw)
{
    return {
        {"stationCapacityCost", static_cast<double>(raw.stationCapacityCost)},
        {"taskPrecedenceCost", static_cast<double>(raw.taskPrecedenceCost)},
        {"maxStationsCost", static_cast<double>(raw.maxStationsCost)},
        {"taskStationBoundsCost", static_cast<double>(raw.taskStationBoundsCost)},
        {"fixedTaskStationsCost", static_cast<double>(raw.fixedTaskStationsCost)},
        {"forbiddenTaskStationsCost", static_cast<double>(raw.forbiddenTaskStationsCost)},
        {"sameStationGroupsCost", static_cast<double>(raw.sameStationGroupsCost)},
        {"differentStationPairsCost", static_cast<double>(raw.differentStationPairsCost)},
        {"usedStationsCost", static_cast<double>(raw.usedStationsCost)},
        {"workloadImbalanceCost", static_cast<double>(raw.workloadImbalanceCost)},
    };
}

// Compares the maintained per-constraint totals of one walk state with a
// full recompute: prints the first mismatching constraint (at most
// `printBudget` times) and records every constraint that is non-zero.
// Returns true when every constraint agrees.
inline bool sameTotals(const std::map<std::string, double> &maintained,
                       const std::map<std::string, double> &recomputed,
                       std::map<std::string, int> &nonZeroStates,
                       const std::string &where,
                       int &printBudget)
{
    bool same = maintained.size() == recomputed.size();
    for (const auto &[name, cost] : recomputed)
    {
        if (cost != 0.0)
            ++nonZeroStates[name];
        const auto it = maintained.find(name);
        if (it == maintained.end() || std::fabs(it->second - cost) > 1e-9)
        {
            if (same && printBudget-- > 0)
            {
                std::printf("  %s MISMATCH %s: maintained=%g recomputed=%g\n", name.c_str(),
                            where.c_str(), it == maintained.end() ? NAN : it->second, cost);
            }
            same = false;
        }
    }
    return same;
}

// True (and silent) when every name of `names` was non-zero in at least one
// walk state; otherwise prints the names that never were.
inline bool everyConstraintNonZero(const std::map<std::string, int> &nonZeroStates,
                                   const std::vector<std::string> &names)
{
    bool all = true;
    for (const std::string &name : names)
    {
        const auto it = nonZeroStates.find(name);
        if (it == nonZeroStates.end() || it->second == 0)
        {
            std::printf("  %s was never non-zero during the walk\n", name.c_str());
            all = false;
        }
    }
    return all;
}

inline Task task(const std::string &id,
                 std::int64_t duration,
                 int station = -1,
                 int minStation = 0,
                 std::optional<int> maxStation = std::nullopt,
                 std::optional<int> fixedStation = std::nullopt,
                 std::vector<int> forbidden = {})
{
    Task t;
    t.id = id;
    t.duration = duration;
    t.station = station;
    t.minStation = minStation;
    t.maxStation = maxStation;
    t.fixedStation = fixedStation;
    t.forbiddenStations = std::move(forbidden);
    return t;
}

// The hand-calculated fixture H (every constraint positive):
//   cycle 5, max_stations 3
//   A: duration 3, station 1, fixed 0, forbidden [1, 1]
//   B: duration 3, station 0, min_station 1
//   C: duration 2, station 1, fixed 1
//   D: duration 6, station 3, max_station 2, forbidden [0]
//   arcs A->B, C->D; groups {A, C, C}, {B, A, C}; pairs (A, C), (B, D), (B, B)
// Loads: station 0 = 3, 1 = 5, 2 = 0 (empty middle station), 3 = 6.
// Raw totals:
//   stationCapacityCost        1  (station 3: 6 - 5)
//   taskPrecedenceCost         1  (A on 1 after B on 0)
//   maxStationsCost            1  (D on 3: 3 - 3 + 1)
//   taskStationBoundsCost      2  (B below min 1 by 1; D above max 2 by 1)
//   fixedTaskStationsCost      1  (A on 1, fixed 0; C on its fixed 1)
//   forbiddenTaskStationsCost  1  (A on 1, listed twice: counts once)
//   sameStationGroupsCost      1  ({A, C, C} on {1}: 0; {B, A, C} on {0, 1}: 1)
//   differentStationPairsCost  2  ((A, C) both on 1; (B, B) a self-pair)
//   usedStationsCost           3  (stations 0, 1, 3)
//   workloadImbalanceCost      3  (6 - 3)
// Default score: -10hard / -3soft.
inline AssemblyLineBalancingProblem fixtureH()
{
    AssemblyLineBalancingProblem p;
    p.cycleTime = 5;
    p.maxStations = 3;
    p.tasks = {
        task("A", 3, 1, 0, std::nullopt, 0, {1, 1}),
        task("B", 3, 0, 1),
        task("C", 2, 1, 0, std::nullopt, 1),
        task("D", 6, 3, 0, 2, std::nullopt, {0}),
    };
    p.precedences = {{0, 1}, {2, 3}};
    p.sameStationGroups = {{"g1", {0, 2, 2}}, {"g2", {1, 0, 2}}};
    p.differentStationPairs = {{0, 2}, {1, 3}, {1, 1}};
    return p;
}

// The zero fixture Z: a feasible line on facts of the same kinds (no
// self-pair, D shortened to 2):
//   cycle 5, max_stations 3
//   A: duration 3, station 0, fixed 0, forbidden [1, 1]
//   B: duration 3, station 1, min_station 1
//   C: duration 2, station 0, fixed 0
//   D: duration 2, station 1, max_station 2, forbidden [0]
//   arcs A->B, C->D; groups {A, C, C}, {B, D, D}; pairs (A, B), (C, D)
// Loads 5 / 5: every hard term 0, usedStationsCost 2, workloadImbalanceCost 0.
inline AssemblyLineBalancingProblem fixtureZ()
{
    AssemblyLineBalancingProblem p;
    p.cycleTime = 5;
    p.maxStations = 3;
    p.tasks = {
        task("A", 3, 0, 0, std::nullopt, 0, {1, 1}),
        task("B", 3, 1, 1),
        task("C", 2, 0, 0, std::nullopt, 0),
        task("D", 2, 1, 0, 2, std::nullopt, {0}),
    };
    p.precedences = {{0, 1}, {2, 3}};
    p.sameStationGroups = {{"g1", {0, 2, 2}}, {"g2", {1, 3, 3}}};
    p.differentStationPairs = {{0, 1}, {2, 3}};
    return p;
}

// A random instance with every feature: duplicate and self arcs (the graph
// may be cyclic; only scoring uses it), groups of 2..4 with repeats, pairs
// with self-pairs, station domains, fixed and forbidden stations (repeats),
// an optional max_stations, and stations in [-1, n + 1].
inline AssemblyLineBalancingProblem randomInstance(int taskCount, std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    auto below = [&rng](int bound) { return static_cast<int>(rng() % static_cast<std::uint64_t>(bound)); };
    AssemblyLineBalancingProblem p;
    p.cycleTime = 5 + below(6);
    if (below(2) == 0)
        p.maxStations = 1 + below(taskCount);
    for (int t = 0; t < taskCount; ++t)
    {
        Task x = task("t" + std::to_string(t), 1 + below(6));
        x.minStation = below(3);
        if (below(3) == 0)
            x.maxStation = x.minStation + below(taskCount);
        if (below(4) == 0)
            x.fixedStation = below(taskCount);
        const int forbidden = below(3);
        for (int k = 0; k < forbidden; ++k)
            x.forbiddenStations.push_back(below(taskCount + 1));
        x.station = below(taskCount + 3) - 1;
        p.tasks.push_back(x);
    }
    const int arcs = below(2 * taskCount + 1);
    for (int a = 0; a < arcs; ++a)
        p.precedences.push_back({below(taskCount), below(taskCount)});
    const int groups = below(4);
    for (int g = 0; g < groups; ++g)
    {
        TaskGroup group{"g" + std::to_string(g), {}};
        const int size = 2 + below(3);
        for (int k = 0; k < size; ++k)
            group.tasks.push_back(below(taskCount));
        p.sameStationGroups.push_back(group);
    }
    const int pairs = below(taskCount + 1);
    for (int k = 0; k < pairs; ++k)
        p.differentStationPairs.push_back({below(taskCount), below(taskCount)});
    return p;
}

// A SALBP-1-like instance: a layered acyclic precedence graph, durations
// 1..cycle/2, no zoning.
inline AssemblyLineBalancingProblem salbpInstance(int taskCount, std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    AssemblyLineBalancingProblem p;
    p.cycleTime = 20;
    for (int t = 0; t < taskCount; ++t)
        p.tasks.push_back(task("T" + std::to_string(1000 + t), 1 + static_cast<int>(rng() % 10)));
    for (int t = 1; t < taskCount; ++t)
    {
        const int arcs = static_cast<int>(rng() % 3);
        for (int k = 0; k < arcs; ++k)
            p.precedences.push_back({static_cast<int>(rng() % t), t});
    }
    return p;
}

}  // namespace alb_test

#endif  // ALB_TEST_FIXTURES_HPP

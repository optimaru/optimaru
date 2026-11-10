// Shared test fixtures for ks_test and ks_plugin_test: the check() harness,
// the hand-calculated instances and a seeded random instance generator.
#ifndef KS_TEST_FIXTURES_HPP
#define KS_TEST_FIXTURES_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "ks/domain.hpp"

namespace ks_test
{

using namespace ks;

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

inline Item makeItem(const std::string &id, std::int64_t weight, std::int64_t value)
{
    Item item;
    item.id = id;
    item.weight = weight;
    item.value = value;
    return item;
}

inline void select(KnapsackProblem &problem, const std::vector<int> &selected)
{
    for (Item &item : problem.items)
        item.selected = kNotSelected;
    for (int i : selected)
        problem.items[i].selected = kSelected;
}

// The basic instance of the reference tests: capacity 10, A 6/9, B 4/7, C 5/8.
inline KnapsackProblem basicInstance()
{
    KnapsackProblem problem;
    problem.capacity = 10;
    problem.items = {makeItem("A", 6, 9), makeItem("B", 4, 7), makeItem("C", 5, 8)};
    return problem;
}

// The extended instance of the reference tests (test_extended_constraint_costs):
// capacity 10, resource labor 2, mandatory C, forbidden A, group {A, B, C}
// exactly 2, A 3/5 (labor 3, requires B), B 2/4, C 1/1.
inline KnapsackProblem extendedInstance()
{
    KnapsackProblem problem;
    problem.capacity = 10;
    problem.resources.push_back({"labor", 2});
    problem.items = {makeItem("A", 3, 5), makeItem("B", 2, 4), makeItem("C", 1, 1)};
    problem.items[0].resourceUsage["labor"] = 3;
    problem.items[0].requiredItems = {1};
    problem.mandatoryItems = {2};
    problem.forbiddenItems = {0};
    SelectionGroup group;
    group.id = "choose-two";
    group.items = {0, 1, 2};
    group.minSelected = 2;
    group.maxSelected = 2;
    problem.selectionGroups.push_back(group);
    return problem;
}

// A seeded random instance using every kind of fact: resources (with usage
// of undeclared resources too), mandatory / forbidden entries with
// duplicates (never overlapping), groups with and without a maximum, and
// dependencies with duplicates and self-dependencies. The selection is
// random, with `undecided` of the items left at -1. With `sideConstraints`
// false only the capacity and the resources are drawn (a multidimensional
// 0/1 knapsack without mandatory / forbidden items, groups or dependencies).
inline KnapsackProblem randomInstance(int itemCount,
                                      std::uint64_t seed,
                                      double undecided = 0.0,
                                      bool sideConstraints = true)
{
    std::mt19937_64 rng(seed);
    auto pick = [&rng](int lo, int hi)
    { return lo + static_cast<int>(rng() % static_cast<std::uint64_t>(hi - lo + 1)); };
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    KnapsackProblem problem;
    std::int64_t totalWeight = 0;
    for (int i = 0; i < itemCount; ++i)
    {
        Item item = makeItem("I" + std::to_string(i), pick(1, 20), pick(0, 30));
        totalWeight += item.weight;
        problem.items.push_back(item);
    }
    problem.capacity = totalWeight / 3;
    const int resourceCount = pick(0, 3);
    for (int r = 0; r < resourceCount; ++r)
        problem.resources.push_back({"r" + std::to_string(r), pick(0, 3 * itemCount)});
    for (Item &item : problem.items)
    {
        for (int r = 0; r < resourceCount + 1; ++r)  // r == resourceCount: undeclared
        {
            if (unit(rng) < 0.5)
                item.resourceUsage["r" + std::to_string(r)] = pick(0, 9);
        }
        if (sideConstraints && unit(rng) < 0.3)
        {
            const int k = pick(1, 3);
            for (int j = 0; j < k; ++j)
                item.requiredItems.push_back(pick(0, itemCount - 1));  // may be self / repeat
        }
    }
    std::vector<int> role(itemCount, 0);  // 1 mandatory, 2 forbidden
    for (int i = 0; i < itemCount; ++i)
    {
        const double u = unit(rng);
        role[i] = !sideConstraints ? 0 : (u < 0.15 ? 1 : (u < 0.30 ? 2 : 0));
        const int copies = role[i] != 0 ? pick(1, 2) : 0;
        for (int c = 0; c < copies; ++c)
            (role[i] == 1 ? problem.mandatoryItems : problem.forbiddenItems).push_back(i);
    }
    std::shuffle(problem.mandatoryItems.begin(), problem.mandatoryItems.end(), rng);
    std::shuffle(problem.forbiddenItems.begin(), problem.forbiddenItems.end(), rng);
    const int groupCount = sideConstraints ? pick(0, 4) : 0;
    for (int g = 0; g < groupCount; ++g)
    {
        SelectionGroup group;
        group.id = "g" + std::to_string(g);
        std::vector<int> all(itemCount);
        for (int i = 0; i < itemCount; ++i)
            all[i] = i;
        std::shuffle(all.begin(), all.end(), rng);
        const int size = pick(1, std::min(itemCount, 6));
        group.items.assign(all.begin(), all.begin() + size);
        if (unit(rng) < 0.5)
            group.maxSelected = pick(0, size);
        group.minSelected = pick(0, group.effectiveMax());
        problem.selectionGroups.push_back(group);
    }
    for (Item &item : problem.items)
    {
        item.selected = unit(rng) < undecided ? kUndecided : (unit(rng) < 0.5 ? kSelected : kNotSelected);
    }
    return problem;
}

}  // namespace ks_test

#endif  // KS_TEST_FIXTURES_HPP

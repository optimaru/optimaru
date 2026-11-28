// Plugin-assembly (cl/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedObjective reproduces
//       ScoreCalculator's full recompute on 300 random loadings (items in no
//       list included)
//   (b) incremental score == full recompute across 3000 random sequence moves
//       driven through the engine's Editor protocol
//   (c) each component's raw cost, hand-calculated: a positive and a zero
//       case per constraint, read through constraint_costs()
//   (a), (b), (g) and (h) run on problems with every input in use: stacking
//       rules, layer limits, bottomOnly, a stack height and balance limits
//       too
//   (d) the catalog: info() off the default assembly == the components'
//       kDefaultWeight / kDefaultLevel; the level and weight guards
//   (e) weight / level overrides, only() and drop(), the opt-in costs picked
//   (f) constraint_totals() explains exactly the reported score
//   (g) every move preview == the applied score under ScoreChecks::Full, with
//       every constraint weighted, repeated and shifted components included
//   (h) a step-limited solve(problem, calculator) under ScoreChecks::Full; the
//       score it reports == a fresh recompute of the same assembly
//   (i) groupSplitCost's previews when both changed containers hold the same
//       group, by hand: a group leaving one container, moving between two,
//       and two groups changing at once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cl/constraints.hpp"
#include "cl/model/demo_data.hpp"
#include "cl/model/domain.hpp"
#include "cl/score/score_calculator.hpp"
#include "cl/solver.hpp"
#include "optimaru/solution/editor.hpp"
#include "optimaru/testing/check.hpp"
#include "optimaru/testing/preview_parity.hpp"

using namespace cl;

namespace {

using optimaru::testing::check;
using optimaru::testing::failures;

bool sameScore(const Score& a, const Score& b) {
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

void checkScore(const std::string& label, const Score& actual, long long expectedHard,
                double expectedSoft) {
    const bool ok = (actual.hard == expectedHard && std::fabs(actual.soft - expectedSoft) < 1e-9);
    std::printf("  [%s] %-58s got=%s expected=%lldhard/%gsoft\n", ok ? "PASS" : "FAIL",
                label.c_str(), actual.str().c_str(), expectedHard, expectedSoft);
    if (!ok) ++failures;
}

int addContainer(ContainerLoadingProblem& p, int length, int width, int height) {
    Container container;
    container.id = "c" + std::to_string(p.containers.size());
    container.length = length;
    container.width = width;
    container.height = height;
    p.containers.push_back(container);
    return static_cast<int>(p.containers.size()) - 1;
}

int addItem(ContainerLoadingProblem& p, int length, int width, int height,
            std::int64_t weight = 0) {
    Item item;
    item.id = "i" + std::to_string(p.items.size());
    item.length = length;
    item.width = width;
    item.height = height;
    item.weight = weight;
    item.rotations = Rotation::Fixed;
    p.items.push_back(item);
    return static_cast<int>(p.items.size()) - 1;
}

Item& itemOf(ContainerLoadingProblem& p, int index) {
    return p.items[static_cast<std::size_t>(index)];
}

Score scoreWith(DecomposedCalculator& calculator, ContainerLoadingProblem& p) {
    ScoreCalculator::prepare(p);
    calculator.rebuild(p);
    return calculator.score();
}

// A problem every constraint has something to say about: weight limits,
// stacking limits, unloading orders, groups, containers of two sizes and
// costs; and, drawn from a generator of their own, stacking rules, layer
// limits, bottomOnly items, a stack height and balance limits.
ContainerLoadingProblem busyProblem(std::uint64_t seed, int containers = 4, int items = 36) {
    std::mt19937_64 rng(seed);
    ContainerLoadingProblem p;
    for (int c = 0; c < containers; ++c) {
        addContainer(p, c % 2 == 0 ? 12 : 9, 8, 9);
        p.containers.back().maxWeight = 40 + static_cast<std::int64_t>(rng() % 30);
        p.containers.back().cost = c % 2 == 0 ? 3.0 : 2.0;
    }
    for (int i = 0; i < items; ++i) {
        const auto side = [&rng] { return 1 + static_cast<int>(rng() % 5); };
        const int index =
                addItem(p, side(), side(), side(), 1 + static_cast<std::int64_t>(rng() % 9));
        itemOf(p, index).rotations = static_cast<Rotation>(rng() % 3);
        if (rng() % 3 == 0) itemOf(p, index).maxLoadOnTop = static_cast<std::int64_t>(rng() % 10);
        if (rng() % 2 == 0) itemOf(p, index).unloadOrder = static_cast<int>(rng() % 4);
        if (rng() % 2 == 0) itemOf(p, index).group = "g" + std::to_string(rng() % 6);
    }
    std::mt19937_64 more(seed + 1000);
    for (Item& item : p.items) {
        item.bottomOnly = more() % 10 == 0;
        if (more() % 4 == 0) item.maxStackLayers = 1 + static_cast<int>(more() % 3);
        if (more() % 2 == 0) item.stackGroup = "s" + std::to_string(more() % 3);
    }
    p.stackingRules = {{"s0", "s1"}, {"s1", "s1"}};
    p.containers[0].maxStackHeight = 7;
    p.containers[1].maxOffsetLength = 0.2;
    p.containers[2].maxOffsetWidth = 0.3;
    ScoreCalculator::prepare(p);
    return p;
}

// Every item in a random list, at a random place; `leaveOut` of them in none.
void scatter(ContainerLoadingProblem& p, std::mt19937_64& rng, double leaveOut) {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (Container& container : p.containers) container.items.clear();
    for (std::size_t i = 0; i < p.items.size(); ++i) {
        if (unit(rng) < leaveOut) continue;
        std::vector<int>& list = p.containers[rng() % p.containers.size()].items;
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(rng() % (list.size() + 1)),
                    static_cast<int>(i));
    }
}

// -------------------------------------------------------------------------
// (a) The default assembly == the full recompute, on random loadings.
// -------------------------------------------------------------------------
void verifyAssemblyParity() {
    std::printf("== (a) Default assembly == ScoreCalculator ==\n");
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    std::mt19937_64 rng(99);
    int mismatches = 0;
    long long hardSeen = 0;
    const int rounds = 300;
    for (int round = 0; round < rounds; ++round) {
        ContainerLoadingProblem p = busyProblem(1000 + static_cast<std::uint64_t>(round % 10));
        scatter(p, rng, 0.10);
        const Score plugin = scoreWith(calculator, p);
        const Score full = ScoreCalculator::score(p);
        hardSeen += -full.hard;
        if (!sameScore(plugin, full)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @round %d: plugin=%s full=%s\n", round,
                            plugin.str().c_str(), full.str().c_str());
            }
        }
    }
    check(mismatches == 0, std::to_string(rounds) + " random loadings score identically");
    check(hardSeen > 1000, "the loadings break hard constraints, so the parity is not of zeros");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental == full recompute across random moves.
// -------------------------------------------------------------------------
void verifyIncremental() {
    std::printf("== (b) Incremental == full recompute ==\n");
    ContainerLoadingProblem p = busyProblem(7);
    const int itemCount = static_cast<int>(p.items.size());
    const int containerCount = static_cast<int>(p.containers.size());

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::Editor<ContainerLoadingProblem> editor(p, calculator);

    std::mt19937_64 rng(2024);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<int> listOf(static_cast<std::size_t>(itemCount), -1);
    std::vector<int> unassigned;
    for (int i = 0; i < itemCount; ++i) unassigned.push_back(i);
    const auto indexWithin = [&p](int container, int item) {
        const std::vector<int>& list = p.containers[static_cast<std::size_t>(container)].items;
        return static_cast<int>(std::find(list.begin(), list.end(), item) - list.begin());
    };
    const auto anyPosition = [&p, &rng](int container) {
        return static_cast<int>(
                rng() % (p.containers[static_cast<std::size_t>(container)].items.size() + 1));
    };

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step) {
        const double dice = unit(rng);
        if (dice < 0.35 && !unassigned.empty()) {
            const std::size_t pick = rng() % unassigned.size();
            const int item = unassigned[pick];
            unassigned.erase(unassigned.begin() + static_cast<std::ptrdiff_t>(pick));
            const int container = static_cast<int>(rng() % containerCount);
            editor.insert(container, anyPosition(container), item);
            listOf[static_cast<std::size_t>(item)] = container;
        } else if (dice < 0.50) {
            const int item = static_cast<int>(rng() % itemCount);
            const int from = listOf[static_cast<std::size_t>(item)];
            if (from < 0) continue;
            editor.remove(from, indexWithin(from, item));
            listOf[static_cast<std::size_t>(item)] = -1;
            unassigned.push_back(item);
        } else if (dice < 0.85) {
            const int item = static_cast<int>(rng() % itemCount);
            const int from = listOf[static_cast<std::size_t>(item)];
            if (from < 0) continue;
            editor.remove(from, indexWithin(from, item));
            const int container = static_cast<int>(rng() % containerCount);
            editor.insert(container, anyPosition(container), item);
            listOf[static_cast<std::size_t>(item)] = container;
        } else {
            const int container = static_cast<int>(rng() % containerCount);
            const int size = static_cast<int>(
                    p.containers[static_cast<std::size_t>(container)].items.size());
            if (size < 2) continue;
            int first = static_cast<int>(rng() % size);
            int last = static_cast<int>(rng() % size);
            if (first > last) std::swap(first, last);
            editor.reverse(container, first, last);
        }
        const Score incremental = editor.score();
        ContainerLoadingProblem copy = p;
        const Score full = ScoreCalculator::score(copy);
        if (!sameScore(full, incremental)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n", step,
                            incremental.str().c_str(), full.str().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps) + " random list moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Each component's raw cost on hand-built instances, read through
// constraint_costs(): the raw per-container numbers before weight and level.
// -------------------------------------------------------------------------
std::vector<double> rawCosts(ContainerLoadingProblem& p, const std::string& name) {
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    scoreWith(calculator, p);
    for (const auto& entry : calculator.constraint_costs()) {
        if (entry.name == name) return entry.unit_costs;
    }
    return {};
}

void verifyRawCostsByHand() {
    std::printf("== (c) Raw costs by hand, positive and zero ==\n");
    {
        // unplacedItemCost: three cubes of 5 along a space of 10 x 5 x 5.
        ContainerLoadingProblem p;
        addContainer(p, 10, 5, 5);
        addContainer(p, 10, 5, 5);
        p.containers[0].items = {addItem(p, 5, 5, 5), addItem(p, 5, 5, 5), addItem(p, 5, 5, 5)};
        check(rawCosts(p, "unplacedItemCost") == std::vector<double>{1.0, 0.0},
              "unplacedItemCost: the third cube finds no place");
        p.containers[1].items = {p.containers[0].items.back()};
        p.containers[0].items.pop_back();
        check(rawCosts(p, "unplacedItemCost") == std::vector<double>{0.0, 0.0},
              "unplacedItemCost: 0 once it is in the other container");
    }
    {
        // containerWeightCost: 8 + 7 against a limit of 10, and no limit.
        ContainerLoadingProblem p;
        addContainer(p, 10, 10, 10);
        addContainer(p, 10, 10, 10);
        p.containers[0].maxWeight = 10;
        p.containers[0].items = {addItem(p, 2, 2, 2, 8), addItem(p, 2, 2, 2, 7)};
        p.containers[1].items = {addItem(p, 2, 2, 2, 900)};
        check(rawCosts(p, "containerWeightCost") == std::vector<double>{5.0, 0.0},
              "containerWeightCost: 5 over the limit; no limit, no cost");
        p.containers[0].maxWeight = 15;
        check(rawCosts(p, "containerWeightCost") == std::vector<double>{0.0, 0.0},
              "containerWeightCost: 0 at the limit");
    }
    {
        // stackingLoadCost: 8 on an item that carries 5.
        ContainerLoadingProblem p;
        addContainer(p, 4, 4, 8);
        const int below = addItem(p, 4, 4, 4, 3);
        const int above = addItem(p, 4, 4, 4, 8);
        itemOf(p, below).maxLoadOnTop = 5;
        p.containers[0].items = {below, above};
        check(rawCosts(p, "stackingLoadCost") == std::vector<double>{3.0},
              "stackingLoadCost: 3 above what the item carries");
        p.containers[0].items = {above, below};
        check(rawCosts(p, "stackingLoadCost") == std::vector<double>{0.0},
              "stackingLoadCost: 0 with the limited item on top");
    }
    {
        // unloadingOrderCost: stops 1, 2, 3 in a row; loaded 1, 2, 3 every
        // later item is in the way of every earlier one.
        ContainerLoadingProblem p;
        addContainer(p, 12, 4, 4);
        std::vector<int> items;
        for (int stop : {1, 2, 3}) {
            items.push_back(addItem(p, 4, 4, 4));
            itemOf(p, items.back()).unloadOrder = stop;
        }
        p.containers[0].items = items;
        check(rawCosts(p, "unloadingOrderCost") == std::vector<double>{3.0},
              "unloadingOrderCost: three blocking pairs in a row loaded first stop first");
        std::reverse(items.begin(), items.end());
        p.containers[0].items = items;
        check(rawCosts(p, "unloadingOrderCost") == std::vector<double>{0.0},
              "unloadingOrderCost: 0 loaded last stop first");
    }
    {
        // stackingRuleCost: "b" may not stand on "a"; a layer limit of 2 on
        // the bottom of a column of 3.
        ContainerLoadingProblem p;
        addContainer(p, 4, 4, 12);
        addContainer(p, 4, 4, 12);
        p.containers[0].items = {addItem(p, 4, 4, 4), addItem(p, 4, 4, 4)};
        p.containers[1].items = {addItem(p, 4, 4, 4), addItem(p, 4, 4, 4), addItem(p, 4, 4, 4)};
        itemOf(p, 0).stackGroup = "a";
        itemOf(p, 1).stackGroup = "b";
        itemOf(p, 2).maxStackLayers = 2;
        p.stackingRules = {{"b", "a"}};
        check(rawCosts(p, "stackingRuleCost") == std::vector<double>({1.0, 1.0}),
              "stackingRuleCost: one forbidden pair; one layer too many");
        p.stackingRules = {{"a", "b"}};
        itemOf(p, 2).maxStackLayers = 3;
        check(rawCosts(p, "stackingRuleCost") == std::vector<double>({0.0, 0.0}),
              "stackingRuleCost: 0 with the rule the other way round and room for 3 layers");
    }
    {
        // balanceLimitCost: a 4 x 4 item in the corner of an 8 x 8 floor is
        // half of the half length off; a limit of 0.1 along x: 400.
        ContainerLoadingProblem p;
        addContainer(p, 8, 8, 8);
        addContainer(p, 8, 8, 8);
        p.containers[0].maxOffsetLength = 0.1;
        p.containers[0].items = {addItem(p, 4, 4, 4, 5)};
        p.containers[1].items = {addItem(p, 4, 4, 4, 5)};
        check(rawCosts(p, "balanceLimitCost") == std::vector<double>({400.0, 0.0}),
              "balanceLimitCost: 400 thousandths over; a container without a limit costs 0");
        p.containers[0].maxOffsetLength = 0.5;
        check(rawCosts(p, "balanceLimitCost") == std::vector<double>({0.0, 0.0}),
              "balanceLimitCost: 0 at the limit");
    }
    {
        // usedContainerCost: the container's cost when something stands in it.
        ContainerLoadingProblem p;
        addContainer(p, 4, 4, 4);
        addContainer(p, 4, 4, 4);
        addContainer(p, 4, 4, 4);
        p.containers[0].cost = 2.5;
        p.containers[0].items = {addItem(p, 4, 4, 4)};
        p.containers[2].items = {addItem(p, 5, 5, 5)};
        check(rawCosts(p, "usedContainerCost") == std::vector<double>({2.5, 0.0, 0.0}),
              "usedContainerCost: its cost; an empty container and one with only an unplaced "
              "item cost 0");
    }
    {
        // usedLengthCost and loadBalanceCost.
        ContainerLoadingProblem p;
        addContainer(p, 8, 8, 8);
        addContainer(p, 8, 8, 8);
        p.containers[0].items = {addItem(p, 4, 4, 4, 5)};
        check(rawCosts(p, "usedLengthCost") == std::vector<double>({4.0, 0.0}),
              "usedLengthCost: the load's far end; 0 for an empty container");
        check(rawCosts(p, "loadBalanceCost") == std::vector<double>({500.0, 0.0}),
              "loadBalanceCost: 500 per mille for a corner load; 0 for an empty container");
        p.containers[1].items = {addItem(p, 8, 8, 2, 5)};
        check(rawCosts(p, "loadBalanceCost") == std::vector<double>({500.0, 0.0}),
              "loadBalanceCost: 0 for a centred load");
    }
    {
        // groupSplitCost: a global, one total.
        ContainerLoadingProblem p;
        for (int c = 0; c < 3; ++c) addContainer(p, 10, 10, 10);
        std::vector<int> order;
        for (int i = 0; i < 3; ++i) {
            order.push_back(addItem(p, 2, 2, 2));
            itemOf(p, order.back()).group = "order";
        }
        const int alone = addItem(p, 2, 2, 2);
        itemOf(p, alone).group = "single";
        const int none = addItem(p, 2, 2, 2);
        p.containers[0].items = {order[0], alone};
        p.containers[1].items = {order[1], none};
        p.containers[2].items = {order[2]};
        check(rawCosts(p, "groupSplitCost") == std::vector<double>{2.0},
              "groupSplitCost: a group in three containers costs 2");
        p.containers[0].items = {order[0], order[1], order[2]};
        p.containers[1].items = {alone};
        p.containers[2].items = {none};
        check(rawCosts(p, "groupSplitCost") == std::vector<double>{0.0},
              "groupSplitCost: 0 with the group together");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) The catalog.
// -------------------------------------------------------------------------
bool throwsInvalid(const std::function<void()>& call) {
    try {
        call();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

template <typename Component>
bool rowIs(const optimaru::ConstraintInfo& row, const std::string& name, optimaru::Level level,
           double weight, bool global) {
    return row.name == name && row.default_level == Component::kDefaultLevel &&
           row.default_weight == Component::kDefaultWeight && row.default_level == level &&
           row.default_weight == weight && row.level_flippable && !row.reward &&
           row.is_global == global && row.default_enabled == (weight > 0.0);
}

void verifyCatalog() {
    std::printf("== (d) Catalog and guards ==\n");
    const auto rows = defaultConstraints().info();
    const optimaru::Level hard = optimaru::Level::Hard;
    const optimaru::Level soft = optimaru::Level::Soft;
    check(rows.size() == 10 &&
                  rowIs<UnplacedItemCost>(rows[0], "unplacedItemCost", hard, 1.0, false) &&
                  rowIs<ContainerWeightCost>(rows[1], "containerWeightCost", hard, 1.0, false) &&
                  rowIs<StackingLoadCost>(rows[2], "stackingLoadCost", hard, 1.0, false) &&
                  rowIs<StackingRuleCost>(rows[3], "stackingRuleCost", hard, 1.0, false) &&
                  rowIs<UnloadingOrderCost>(rows[4], "unloadingOrderCost", hard, 1.0, false) &&
                  rowIs<BalanceLimitCost>(rows[5], "balanceLimitCost", hard, 1.0, false),
          "info(): six hard constraints at weight 1");
    check(rows.size() == 10 &&
                  rowIs<UsedContainerCost>(rows[6], "usedContainerCost", soft, 1.0, false) &&
                  rowIs<UsedLengthCost>(rows[7], "usedLengthCost", soft, 0.0, false) &&
                  rowIs<LoadBalanceCost>(rows[8], "loadBalanceCost", soft, 0.0, false) &&
                  rowIs<GroupSplitCost>(rows[9], "groupSplitCost", soft, 1.0, true),
          "info(): four soft ones, two of them opt-in at weight 0, the global one last");
    check(!throwsInvalid([] { defaultConstraints().soft("unplacedItemCost"); }) &&
                  !throwsInvalid([] { defaultConstraints().hard("groupSplitCost"); }),
          "every level flips both ways");
    check(throwsInvalid([] { defaultConstraints().weight("usedContainerCost", -1.0); }),
          "a negative weight is refused");
    check(throwsInvalid([] { defaultConstraints().weight("usedContainersCost", 1.0); }),
          "a misspelled constraint name throws instead of doing nothing");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) Overrides and partial assemblies.
// -------------------------------------------------------------------------
void verifyOverrides() {
    std::printf("== (e) Overrides and partial assemblies ==\n");
    // Container 0 (cost 2, limit 10): two items of weight 8 and 7, the load 8
    // long. Container 1: one item of a group whose other item is in 0.
    ContainerLoadingProblem p;
    addContainer(p, 10, 4, 4);
    addContainer(p, 10, 4, 4);
    p.containers[0].cost = 2.0;
    p.containers[0].maxWeight = 10;
    const int a = addItem(p, 4, 4, 4, 8);
    const int b = addItem(p, 4, 4, 4, 7);
    const int c = addItem(p, 4, 4, 4, 1);
    itemOf(p, a).group = "order";
    itemOf(p, c).group = "order";
    p.containers[0].items = {a, b};
    p.containers[1].items = {c};
    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        checkScore("the defaults: 5 overweight, containers 2 + 1, one split",
                   scoreWith(calculator, p), -5, -4.0);
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints()
                .weight("usedContainerCost", 10.0)
                .weight("containerWeightCost", 3.0)
                .into(calculator);
        checkScore("weights: containers x 10, overweight x 3", scoreWith(calculator, p), -15,
                   -31.0);
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().soft("containerWeightCost").hard("groupSplitCost").into(calculator);
        checkScore("levels flipped: the split hard, the overweight soft", scoreWith(calculator, p),
                   -1, -8.0);
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints()
                .weight("usedLengthCost", 0.5)
                .weight("loadBalanceCost", 0.01)
                .into(calculator);
        // Lengths 8 and 4; balance: container 0's load spans x 0..8 of 10 with
        // the heavier item at the back (centre of gravity x = 58/15), container
        // 1's sits at x 0..4.
        const double balance0 = std::floor(1000.0 * (5.0 - 58.0 / 15.0) / std::sqrt(29.0) + 0.5);
        const double balance1 = std::floor(1000.0 * 3.0 / std::sqrt(29.0) + 0.5);
        checkScore("the opt-in costs picked: length x 0.5, balance x 0.01",
                   scoreWith(calculator, p), -5,
                   -(4.0 + 0.5 * 12.0 + 0.01 * (balance0 + balance1)));
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().only({"usedContainerCost"}).into(calculator);
        checkScore("only() scores exactly the named constraint", scoreWith(calculator, p), 0, -3.0);
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().drop("groupSplitCost").drop("containerWeightCost").into(calculator);
        checkScore("drop() removes a unit constraint and the global one", scoreWith(calculator, p),
                   0, -3.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) constraint_totals() explains the score.
// -------------------------------------------------------------------------
void verifyConstraintTotals() {
    std::printf("== (f) constraint_totals() explains the score ==\n");
    ContainerLoadingProblem p = busyProblem(5);
    std::mt19937_64 rng(5);
    scatter(p, rng, 0.0);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    const Score score = scoreWith(calculator, p);
    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal& total : calculator.constraint_totals())
        (total.is_hard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9 &&
                  std::fabs(-soft - score.soft) < 1e-6,
          "per-constraint totals sum to the reported score");
    check(calculator.constraint_totals().size() == 10, "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) Previews, with every constraint weighted and some repeated.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string& name) {
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

ConstraintAssembly weightedAssembly() {
    ConstraintAssembly assembly = defaultConstraints();
    assembly.weight("usedLengthCost", 0.25).weight("loadBalanceCost", 0.01);
    assembly.offset("usedLengthCost", 3.0);
    assembly.add_instances(std::move(copyOf("usedLengthCost")
                                             .weight("usedLengthCost", 0.05)
                                             .exponent("usedLengthCost", 2.0)));
    assembly.add_instances(std::move(copyOf("unplacedItemCost")
                                             .soft("unplacedItemCost")
                                             .weight("unplacedItemCost", 7.0)
                                             .offset("unplacedItemCost", 1.0)
                                             .clip("unplacedItemCost", false)));
    assembly.add_instances(std::move(copyOf("groupSplitCost")
                                             .weight("groupSplitCost", 2.0)
                                             .exponent("groupSplitCost", 2.0)));
    assembly.add_instances(std::move(copyOf("balanceLimitCost")
                                             .soft("balanceLimitCost")
                                             .weight("balanceLimitCost", 0.01)
                                             .offset("balanceLimitCost", 50.0)
                                             .clip("balanceLimitCost", false)));
    assembly.add_instances(std::move(copyOf("stackingRuleCost")
                                             .soft("stackingRuleCost")
                                             .weight("stackingRuleCost", 3.0)
                                             .exponent("stackingRuleCost", 2.0)));
    assembly.add_instances(std::move(copyOf("stackingLoadCost")
                                             .soft("stackingLoadCost")
                                             .weight("stackingLoadCost", 0.5)
                                             .offset("stackingLoadCost", -2.0)));
    return assembly;
}

void verifyPreviews() {
    std::printf("== (g) Every preview == the applied score ==\n");
    for (const bool weighted : {false, true}) {
        ContainerLoadingProblem p = busyProblem(weighted ? 21 : 20);
        std::mt19937_64 rng(31);
        scatter(p, rng, 0.0);
        DecomposedCalculator calculator;
        if (weighted) {
            weightedAssembly().into(calculator);
        } else {
            addDefaultConstraints(calculator);
        }
        optimaru::Editor<ContainerLoadingProblem> editor(p, calculator,
                                                         optimaru::ScoreChecks::Full);
        editor.score();
        check(calculator.has_previews(), "the assembly serves previews (the global one included)");
        optimaru::testing::PreviewParity parity;
        bool corrupted = false;
        try {
            parity = optimaru::testing::run_preview_parity(editor, calculator, /*seed=*/13,
                                                           /*trials=*/2000);
        } catch (const optimaru::ScoreMismatchError& error) {
            corrupted = true;
            std::printf("    %s\n", error.what());
        }
        std::printf("    %d moves checked, %d previewed, %d declined\n", parity.checked,
                    parity.previewed, parity.declined);
        check(!corrupted && parity.all_equal() && parity.declined == 0 && parity.previewed > 1000,
              std::string(weighted ? "repeated, shifted components" : "the default assembly") +
                      ": every preview served and equal to the applied score (FULL_ASSERT)");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) groupSplitCost previews where both changed units share a group.
// -------------------------------------------------------------------------
void verifyGroupSplitPreviews() {
    std::printf("== (i) groupSplitCost previews, two units sharing a group ==\n");
    ContainerLoadingProblem p;
    for (int c = 0; c < 3; ++c) addContainer(p, 10, 10, 10);
    const auto item = [&p](const char* group) {
        const int index = addItem(p, 2, 2, 2);
        if (group != nullptr) itemOf(p, index).group = group;
        return index;
    };
    const int g0 = item("g"), g1 = item("g"), g2 = item("g");
    const int h0 = item("h"), h1 = item("h");
    const int x = item(nullptr), y = item(nullptr), z = item(nullptr);
    // A = [g0 g1 x], B = [g2 y h0], C = [z h1]: g in A and B, h in B and C.
    p.containers[0].items = {g0, g1, x};
    p.containers[1].items = {g2, y, h0};
    p.containers[2].items = {z, h1};
    ScoreCalculator::prepare(p);

    DecomposedCalculator calculator;
    defaultConstraints().only({"groupSplitCost"}).into(calculator);
    optimaru::Editor<ContainerLoadingProblem> editor(p, calculator, optimaru::ScoreChecks::Full);
    checkScore("start: g and h are each split once", editor.score(), 0, -2.0);

    const auto setLists = [&](const std::vector<std::vector<int>>& lists) {
        for (std::size_t c = 0; c < lists.size(); ++c) {
            if (p.containers[c].items == lists[c]) continue;
            editor.begin_list_edit(static_cast<int>(c));
            p.containers[c].items = lists[c];
            editor.end_list_edit(static_cast<int>(c));
        }
    };
    const std::vector<std::vector<int>> start = {p.containers[0].items, p.containers[1].items,
                                                 p.containers[2].items};
    // Check a served preview against the applied move and the hand count, then undo.
    const auto expect = [&](const std::string& label, bool served, const Score& preview,
                            const std::vector<std::vector<int>>& after, double soft) {
        check(served, label + ": the preview is served");
        setLists(after);
        const Score applied = editor.score();
        check(sameScore(preview, applied), label + ": preview == applied score");
        checkScore(label, applied, 0, soft);
        setLists(start);
    };

    Score preview;
    // g leaves B for A; x goes the other way: g is together, h still split.
    bool served = calculator.preview_swap(0, 2, 1, 0, preview);
    expect("swap g2 <-> x: g joins A", served, preview, {{g0, g1, g2}, {x, y, h0}, {z, h1}}, -1.0);
    // g's B item moves to C: g leaves B and enters C, the -1 and +1 cancel.
    served = calculator.preview_segment_move(1, 0, 1, 2, 0, preview);
    expect("move g2 B -> C: g still split", served, preview, {{g0, g1, x}, {y, h0}, {g2, z, h1}},
           -2.0);
    // h's B item swaps with C's z: h ends in C alone, both units saw h.
    served = calculator.preview_swap(1, 2, 2, 0, preview);
    expect("swap h0 <-> z: h joins C", served, preview, {{g0, g1, x}, {g2, y, z}, {h0, h1}}, -1.0);
    // [g2 y] from B to A for x: g together in A, h unchanged (B and C).
    served = calculator.preview_exchange(0, 2, 1, 1, 0, 2, preview);
    expect("exchange [x] <-> [g2 y]: g joins A", served, preview,
           {{g0, g1, g2, y}, {x, h0}, {z, h1}}, -1.0);
    // B's g2 for C's h1: g moves from B to C (still split), h gathers in B.
    served = calculator.preview_exchange(1, 0, 1, 2, 1, 1, preview);
    expect("exchange [g2] <-> [h1]: h joins B", served, preview,
           {{g0, g1, x}, {h1, y, h0}, {z, g2}}, -1.0);
    check(editor.score() == Score{0, -2.0}, "the start's score is back after the undos");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) ScoreChecks::Full solves on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve() {
    std::printf("== (h) FULL_ASSERT plugin solve ==\n");
    for (const bool weighted : {false, true}) {
        ContainerLoadingProblem p = busyProblem(weighted ? 41 : 40, 5, 30);
        Solver::Config config;
        config.time_limit = std::chrono::seconds(60);
        config.step_limit = 2500;
        config.seed = 3;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruin_recreate_unimproved_step_trigger = 200;
        config.ruin_recreate_polish_steps = 100;
        DecomposedCalculator calculator;
        if (weighted) {
            weightedAssembly().into(calculator);
        } else {
            addDefaultConstraints(calculator);
        }
        bool corrupted = false;
        try {
            Solver(config).solve(p, calculator);
        } catch (const optimaru::ScoreMismatchError& error) {
            corrupted = true;
            std::printf("  ScoreMismatchError: %s\n", error.what());
        }
        const std::string which =
                weighted ? "repeated, shifted components" : "the default assembly";
        check(!corrupted, "2500 FULL_ASSERT steps with " + which + " without a score mismatch");
        DecomposedCalculator fresh;
        if (weighted) {
            weightedAssembly().into(fresh);
        } else {
            addDefaultConstraints(fresh);
        }
        const Score reported = p.score;
        check(!corrupted && optimaru::scores_agree(reported, scoreWith(fresh, p)),
              "the reported score == a fresh recompute of the same assembly");
        if (!weighted) {
            check(!corrupted && sameScore(reported, ScoreCalculator::score(p)),
                  "and, for the defaults, == ScoreCalculator's full recompute");
        }
    }
    std::printf("\n");
}

}  // namespace

int main() {
    verifyAssemblyParity();
    verifyIncremental();
    verifyRawCostsByHand();
    verifyCatalog();
    verifyOverrides();
    verifyConstraintTotals();
    verifyPreviews();
    verifyFullAssertSolve();
    verifyGroupSplitPreviews();

    if (failures == 0) {
        std::printf("All container-loading plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d container-loading plugin check(s) FAILED.\n", failures);
    return 1;
}

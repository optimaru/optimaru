// Plugin-assembly (bp/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (unassigned items, colours, duplicated conflicts), and
//       each constraint's unit costs sum to the monolith's raw total
//   (b) incremental == full recompute across 5000 random moves driven
//       through the engine's ScoreDirector protocol (plugin path), for the
//       default assembly and for one with exponents != 1 on the delta-path
//       components and fractional hard weights: after every move the score
//       and every unit cost equal a fresh assembly's, and every constraint
//       is non-zero during the walk
//   (c) partial assemblies: removing one component removes exactly that term
//   (d) weight / level / exponent overrides against hand calculation,
//       including the documented per-bin exponent granularity
//   (e) constraintTotals() explains exactly the reported score
//   (f) solve(problem, calculator) under FULL_ASSERT, every component shaped
//       or re-weighted
//   (g) the defaultConstraints() builder: named edits == by-hand assembly
//   (h) each component's raw cost, hand-calculated: a positive and a zero
//       case per constraint (raw 0 = no violation), and the costs shapes
//       (n entries per unit constraint, one for usedBinsCost)
//   (i) the catalog: info() off the default assembly == the components'
//       kDefaultWeight / kDefaultLevel; the knob guards
//   (j) the macro moves (bp/moves.hpp) on the plugin path: EmptyBinMove and
//       RepackMove driven through the engine's move protocol (apply, score,
//       keep or roll back) on the default and the shaped assembly; after
//       every apply and every undo the score and every unit cost equal a
//       fresh assembly's
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "bp/constraints.hpp"
#include "bp/domain.hpp"
#include "bp/moves.hpp"
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
        p.items.push_back(item);
    }
    for (int k = 0; k < conflicts; ++k)
    {
        const int a = static_cast<int>(rng() % n);
        int b = static_cast<int>(rng() % (n - 1));
        if (b >= a)
            ++b;
        p.conflicts.emplace_back("i" + std::to_string(a), "i" + std::to_string(b));
        if (k % 4 == 0)
            p.conflicts.emplace_back("i" + std::to_string(b), "i" + std::to_string(a));
    }
    return p;
}

void randomize(BinPackingProblem &p, std::mt19937_64 &rng, int bins, double unassigned)
{
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (Item &item : p.items)
        item.bin = unit(rng) < unassigned ? -1 : static_cast<int>(rng() % bins);
}

Score scoreWith(DecomposedCalculator &calculator, BinPackingProblem &p)
{
    ScoreCalculator::prepare(p);
    calculator.resetWorkingSolution(p);
    return calculator.calculateScore();
}

const char *const kConstraintNames[] = {"binCapacityCost", "colorConflictCost", "itemConflictCost",
                                        "usedBinsCost"};

// The raw unit costs of every component, in assembly (catalog) order.
std::vector<std::vector<double>> unitCosts(const DecomposedCalculator &calculator)
{
    std::vector<std::vector<double>> costs;
    for (const auto &entry : calculator.constraintCosts())
        costs.push_back(entry.unitCosts);
    return costs;
}

// Each component's unit costs summed: the monolith's RawCosts in list form.
std::vector<double> sums(const std::vector<std::vector<double>> &costs)
{
    std::vector<double> out;
    for (const std::vector<double> &units : costs)
    {
        double total = 0.0;
        for (double cost : units)
            total += cost;
        out.push_back(total);
    }
    return out;
}

std::vector<double> asList(const RawCosts &raw)
{
    return {static_cast<double>(raw.binCapacity),
            static_cast<double>(raw.colorConflict),
            static_cast<double>(raw.itemConflict),
            static_cast<double>(raw.usedBins)};
}

// The defaults, or the defaults shaped so that the two delta-path components
// (colour, conflicts) run at exponent != 1 and two hard weights are
// fractional (rounded per bin).
void assemble(DecomposedCalculator &calculator, bool shaped)
{
    if (!shaped)
    {
        addDefaultConstraints(calculator);
        return;
    }
    defaultConstraints()
        .weight("binCapacityCost", 0.5)
        .weight("colorConflictCost", 1.5)
        .exponent("colorConflictCost", 2.0)
        .exponent("itemConflictCost", 3.0)
        .into(calculator);
}

// -------------------------------------------------------------------------
// (a) default assembly == monolith
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) default assembly == monolithic score ==\n");
    BinPackingProblem p = randomInstance(24, 3, 4, 20);
    std::mt19937_64 rng(9);
    int mismatches = 0;
    int costMismatches = 0;
    for (int trial = 0; trial < 500; ++trial)
    {
        randomize(p, rng, 1 + static_cast<int>(rng() % 8), trial % 5 == 0 ? 0.2 : 0.0);
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        const Score plugin = scoreWith(calculator, p);
        const Score monolith = ScoreCalculator::calculateScore(p);
        if (!sameScore(plugin, monolith) && ++mismatches <= 3)
        {
            std::printf("  MISMATCH trial %d: plugin=%s monolith=%s\n",
                        trial,
                        plugin.toString().c_str(),
                        monolith.toString().c_str());
        }
        if (sums(unitCosts(calculator)) != asList(ScoreCalculator::rawCosts(p)) && ++costMismatches <= 3)
            std::printf("  RAW COST MISMATCH trial %d\n", trial);
    }
    check(mismatches == 0, "500 random assignments, plugin == monolith");
    check(costMismatches == 0, "500 random assignments, each constraint's unit costs sum to the monolith's raw total");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) plugin incremental == full recompute
// -------------------------------------------------------------------------
void walkIncremental(bool shaped)
{
    const std::string label = shaped ? "shaped assembly" : "default assembly";
    BinPackingProblem p = randomInstance(30, 17, 4, 25);
    std::mt19937_64 rng(31);
    randomize(p, rng, 6, 0.0);
    ScoreCalculator::prepare(p);
    DecomposedCalculator calculator;
    assemble(calculator, shaped);
    optimaru::ScoreDirector<BinPackingProblem> director(p, calculator);
    BinPackingProblem shadow = p;
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
            const int item = static_cast<int>(rng() % n);
            director.changeVariable(item, unit(rng) < 0.1 ? -1 : static_cast<int>(rng() % 6));
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
        DecomposedCalculator fresh;
        assemble(fresh, shaped);
        const Score full = scoreWith(fresh, shadow);
        const Score incremental = director.calculateScore();
        // The default assembly must also match the monolith's full recompute.
        const bool monolithAgrees = shaped || sameScore(full, ScoreCalculator::calculateScore(shadow));
        if ((!sameScore(full, incremental) || !monolithAgrees) && ++mismatches <= 3)
        {
            std::printf("  MISMATCH @step %d: incremental=%s full=%s\n",
                        step,
                        incremental.toString().c_str(),
                        full.toString().c_str());
        }
        const std::vector<std::vector<double>> costs = unitCosts(calculator);
        const std::vector<std::vector<double>> expected = unitCosts(fresh);
        bool costsAgree = costs == expected;
        if (!shaped)
            costsAgree = costsAgree && sums(costs) == asList(ScoreCalculator::rawCosts(shadow));
        if (!costsAgree && ++costMismatches <= 3)
            std::printf("  UNIT COST MISMATCH @step %d\n", step);
        for (std::size_t c = 0; c < expected.size() && c < 4; ++c)
        {
            if (std::any_of(expected[c].begin(), expected[c].end(), [](double cost) { return cost != 0.0; }))
                ++nonZero[c];
        }
    }
    check(mismatches == 0,
          label + ": " + std::to_string(steps) + " random moves, plugin incremental == full recompute");
    check(costMismatches == 0, label + ": every unit cost == a fresh assembly's after each move");
    for (int c = 0; c < 4; ++c)
    {
        check(nonZero[c] > 0,
              label + ": " + kConstraintNames[c] + " non-zero at " + std::to_string(nonZero[c]) + " of "
                  + std::to_string(steps) + " steps");
    }
}

void verifyIncremental()
{
    std::printf("== (b) plugin incremental == full recompute, per unit ==\n");
    walkIncremental(false);
    walkIncremental(true);
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) partial assemblies
// -------------------------------------------------------------------------
// C = 10: bin 0 = A8(red) B7(red) C3 -> over 8, one extra red; bin 1 = D2;
// conflict A-B together -> 1; 2 bins.
BinPackingProblem everyTermPositive()
{
    BinPackingProblem p;
    p.binCapacity = 10;
    p.items = {makeItem("A", 8, 0, "red"), makeItem("B", 7, 0, "red"), makeItem("C", 3, 0), makeItem("D", 2, 1)};
    p.conflicts = {{"A", "B"}};
    return p;
}

void verifyPartialAssemblies()
{
    std::printf("== (c) partial assemblies ==\n");
    BinPackingProblem p = everyTermPositive();
    struct Case
    {
        const char *dropped;
        long hard;
        double soft;
    };
    const Case cases[] = {
        {"binCapacityCost", -2, -2.0},
        {"colorConflictCost", -9, -2.0},
        {"itemConflictCost", -9, -2.0},
        {"usedBinsCost", -10, 0.0},
    };
    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-10, -2.0}), "all four: -10hard/-2soft (8 + 1 + 1, 2)");
    }
    for (const Case &c : cases)
    {
        DecomposedCalculator calculator;
        defaultConstraints().drop(c.dropped).into(calculator);
        const Score score = scoreWith(calculator, p);
        check(sameScore(score, Score{c.hard, c.soft}),
              std::string("without ") + c.dropped + ": " + score.toString());
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) overrides
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) weight / level / exponent overrides ==\n");
    {
        // The 0.1.5 hand calculation: {A8, B7}, {C3, D2}; capacity soft x3,
        // usedBins hard x2 -> (-4, -15).
        BinPackingProblem p;
        p.binCapacity = 10;
        p.items = {makeItem("A", 8, 0), makeItem("B", 7, 0), makeItem("C", 3, 1), makeItem("D", 2, 1)};
        DecomposedCalculator calculator;
        defaultConstraints()
            .soft("binCapacityCost")
            .weight("binCapacityCost", 3)
            .hard("usedBinsCost")
            .weight("usedBinsCost", 2)
            .into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-4, -15.0}),
              "capacity soft x3 + usedBins hard x2 -> -4hard/-15soft");
    }
    {
        // Per-bin exponent granularity: red x3 + blue x2 in ONE bin is a
        // raw 3 for that bin, so exponent 2 gives 9 (0.1.5: 2^2 + 1^2 = 5).
        BinPackingProblem p;
        p.binCapacity = 100;
        p.items = {makeItem("R1", 1, 0, "red"),
                   makeItem("R2", 1, 0, "red"),
                   makeItem("R3", 1, 0, "red"),
                   makeItem("B1", 1, 0, "blue"),
                   makeItem("B2", 1, 0, "blue"),
                   makeItem("R4", 1, 1, "red"),
                   makeItem("R5", 1, 1, "red")};
        DecomposedCalculator calculator;
        defaultConstraints().only({"colorConflictCost"}).exponent("colorConflictCost", 2.0).into(calculator);
        check(scoreWith(calculator, p).hard == -(9 + 1), "colorConflictCost exponent 2: bin 0 -> 9, bin 1 -> 1");
    }
    {
        // Two violated pairs in one bin: raw 2 for the bin -> 4 at exponent 2.
        BinPackingProblem p;
        p.binCapacity = 100;
        p.items = {makeItem("A", 1, 0), makeItem("B", 1, 0), makeItem("C", 1, 0)};
        p.conflicts = {{"A", "B"}, {"B", "C"}};
        DecomposedCalculator calculator;
        defaultConstraints().only({"itemConflictCost"}).exponent("itemConflictCost", 2.0).into(calculator);
        check(scoreWith(calculator, p).hard == -4, "itemConflictCost exponent 2: two pairs in one bin -> 4");
    }
    {
        // The global: exponent 2 shapes the bin COUNT (3 ** 2 = 9).
        BinPackingProblem p;
        p.binCapacity = 100;
        p.items = {makeItem("A", 1, 0), makeItem("B", 1, 1), makeItem("C", 1, 2)};
        DecomposedCalculator calculator;
        defaultConstraints().only({"usedBinsCost"}).exponent("usedBinsCost", 2.0).into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{0, -9.0}), "usedBinsCost exponent 2: 3 bins -> 9");
    }
    {
        // Hard rounding per bin: weight 0.5 on bins over by 1 and 3 rounds
        // each bin half away from zero: round(0.5) + round(1.5) = 1 + 2.
        BinPackingProblem p;
        p.binCapacity = 10;
        p.items = {makeItem("A", 11, 0), makeItem("B", 13, 1)};
        DecomposedCalculator calculator;
        defaultConstraints().only({"binCapacityCost"}).weight("binCapacityCost", 0.5).into(calculator);
        check(scoreWith(calculator, p).hard == -3, "fractional hard weight rounds per bin");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals()
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    BinPackingProblem p = randomInstance(24, 13, 3, 12);
    std::mt19937_64 rng(5);
    randomize(p, rng, 5, 0.0);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    const Score score = scoreWith(calculator, p);
    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9 && std::fabs(-soft - score.soft) < 1e-9,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == 4, "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT plugin solve
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    BinPackingProblem p = randomInstance(18, 23, 3, 8);
    ScoreCalculator::prepare(p);
    // Every component off its defaults; the delta-path ones (colour,
    // conflicts) at exponent != 1.
    auto shaped = []
    {
        return defaultConstraints()
            .weight("usedBinsCost", 2)
            .exponent("binCapacityCost", 2.0)
            .exponent("colorConflictCost", 2.0)
            .exponent("itemConflictCost", 3.0);
    };
    DecomposedCalculator calculator;
    shaped().into(calculator);
    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;
    config.ruinRecreateAsMoveWeight = 0.05;
    bool corrupted = false;
    try
    {
        Solver(config).solve(p, calculator);
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        DecomposedCalculator fresh;
        shaped().into(fresh);
        check(sameScore(p.score, scoreWith(fresh, p)), "reported score == a fresh assembly on the result");
        check(std::all_of(p.items.begin(), p.items.end(), [](const Item &item) { return item.bin >= 0; }),
              "every item assigned");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) builder
// -------------------------------------------------------------------------
void verifyAssemblyBuilder()
{
    std::printf("== (g) defaultConstraints() builder ==\n");
    BinPackingProblem p = randomInstance(20, 41, 3, 10);
    std::mt19937_64 rng(2);
    randomize(p, rng, 4, 0.0);

    DecomposedCalculator named;
    defaultConstraints()
        .weight("usedBinsCost", 4)
        .soft("binCapacityCost")
        .drop("colorConflictCost")
        .into(named);

    DecomposedCalculator byHand;
    auto capacity = std::make_unique<BinCapacityCost>();
    capacity->setLevel(optimaru::Level::Soft);
    byHand.add(std::move(capacity));
    byHand.emplace<ItemConflictCost>();
    byHand.emplace<UsedBinsCost>(4.0);

    check(sameScore(scoreWith(named, p), scoreWith(byHand, p)), "named edits == the same assembly built by hand");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) raw costs by hand
// -------------------------------------------------------------------------
std::vector<double> rawCosts(BinPackingProblem p, const std::string &name)
{
    ScoreCalculator::prepare(p);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(p);
    calculator.calculateScore();
    for (const auto &entry : calculator.constraintCosts())
    {
        if (entry.name == name)
            return entry.unitCosts;
    }
    return {};
}

BinPackingProblem fourItems(const std::vector<int> &bins)
{
    BinPackingProblem p;
    p.binCapacity = 10;
    p.items = {makeItem("A", 8), makeItem("B", 7), makeItem("C", 3), makeItem("D", 2)};
    p.setAssignment(bins);
    return p;
}

void verifyRawCostsByHand()
{
    std::printf("== (h) raw costs by hand ==\n");
    // binCapacityCost: one entry per bin 0..n-1.
    check(rawCosts(fourItems({0, 0, 1, 1}), "binCapacityCost") == std::vector<double>{5, 0, 0, 0},
          "binCapacityCost: {A8,B7} over by 5 -> [5, 0, 0, 0]");
    check(rawCosts(fourItems({0, 1, 1, 0}), "binCapacityCost") == std::vector<double>{0, 0, 0, 0},
          "binCapacityCost: both bins exactly full -> [0, 0, 0, 0]");
    // usedBinsCost: one global entry.
    check(rawCosts(fourItems({0, 0, 1, 1}), "usedBinsCost") == std::vector<double>{2},
          "usedBinsCost: 2 bins -> [2]");
    check(rawCosts(fourItems({-1, -1, -1, -1}), "usedBinsCost") == std::vector<double>{0},
          "usedBinsCost: nothing assigned -> [0]");
    {
        BinPackingProblem p;
        p.binCapacity = 20;
        p.items = {makeItem("A", 3, 0, "red"), makeItem("B", 4, 0, "red"), makeItem("C", 5, 0, "blue")};
        p.conflicts = {{"A", "C"}};
        check(rawCosts(p, "colorConflictCost") == std::vector<double>{1, 0, 0},
              "colorConflictCost: red red blue in bin 0 -> [1, 0, 0]");
        check(rawCosts(p, "itemConflictCost") == std::vector<double>{1, 0, 0},
              "itemConflictCost: A-C in bin 0 -> [1, 0, 0]");
        p.setAssignment({0, 1, 1});
        check(rawCosts(p, "colorConflictCost") == std::vector<double>{0, 0, 0},
              "colorConflictCost: the reds split -> [0, 0, 0]");
        check(rawCosts(p, "itemConflictCost") == std::vector<double>{0, 0, 0},
              "itemConflictCost: A and C apart -> [0, 0, 0]");
    }
    {
        BinPackingProblem p;
        p.binCapacity = 100;
        p.items = {makeItem("R1", 1, 1, "red"),
                   makeItem("R2", 1, 1, "red"),
                   makeItem("R3", 1, 1, "red"),
                   makeItem("B1", 1, 1, "blue"),
                   makeItem("B2", 1, 1, "blue")};
        p.conflicts = {{"R1", "B1"}, {"B1", "R1"}, {"R2", "B2"}};
        check(rawCosts(p, "colorConflictCost") == std::vector<double>{0, 3, 0, 0, 0},
              "colorConflictCost: red x3 + blue x2 in bin 1 -> 2 + 1");
        check(rawCosts(p, "itemConflictCost") == std::vector<double>{0, 3, 0, 0, 0},
              "itemConflictCost: a duplicated pair counts per listing -> 3");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) catalog and guards
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
bool rowIs(const optimaru::ConstraintInfo &row, const char *name, optimaru::Level level, double weight, bool global)
{
    return row.name == name && row.defaultLevel == Component::kDefaultLevel
           && row.defaultWeight == Component::kDefaultWeight && row.defaultLevel == level
           && row.defaultWeight == weight && row.levelFlippable && !row.reward && row.isGlobal == global
           && row.defaultEnabled;
}

void verifyCatalog()
{
    std::printf("== (i) catalog and guards ==\n");
    const auto rows = defaultConstraints().info();
    check(rows.size() == 4
              && rowIs<BinCapacityCost>(rows[0], "binCapacityCost", optimaru::Level::Hard, 1.0, false)
              && rowIs<ColorConflictCost>(rows[1], "colorConflictCost", optimaru::Level::Hard, 1.0, false)
              && rowIs<ItemConflictCost>(rows[2], "itemConflictCost", optimaru::Level::Hard, 1.0, false)
              && rowIs<UsedBinsCost>(rows[3], "usedBinsCost", optimaru::Level::Soft, 1.0, true),
          "info(): capacity/colour/conflict hard 1, usedBins soft 1 (global); all flippable, "
          "no reward, all default-enabled");
    check(!throwsInvalid([] { defaultConstraints().soft("binCapacityCost"); })
              && !throwsInvalid([] { defaultConstraints().hard("usedBinsCost"); }),
          "every level flips both ways");
    check(throwsInvalid([] { defaultConstraints().weight("usedBinsCost", -1.0); }), "a negative weight is refused");
    check(throwsInvalid([] { defaultConstraints().exponent("itemConflictCost", 0.0); }),
          "a non-positive exponent is refused");
    check(throwsInvalid([] { defaultConstraints().drop("minimizeBins"); }), "the 0.1.5 names are unknown");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) the macro moves on the plugin path
// -------------------------------------------------------------------------
template <typename Move>
void walkMacroMove(Move &move, const std::string &label, bool shaped)
{
    BinPackingProblem p = randomInstance(40, 43, 5, 30);
    std::mt19937_64 rng(59);
    randomize(p, rng, 40, 0.0);
    ScoreCalculator::prepare(p);
    DecomposedCalculator calculator;
    assemble(calculator, shaped);
    optimaru::ScoreDirector<BinPackingProblem> director(p, calculator);
    optimaru::NearbyNeighborhoods nearby;
    std::vector<int> noLists;
    optimaru::OperatorContext<BinPackingProblem> ctx{director, nearby, noLists, noLists};
    BinPackingProblem shadow = p;
    const int n = p.itemCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    auto agrees = [&]() -> bool
    {
        shadow.setAssignment(p.assignment());
        DecomposedCalculator fresh;
        assemble(fresh, shaped);
        const Score full = scoreWith(fresh, shadow);
        return sameScore(director.calculateScore(), full) && unitCosts(calculator) == unitCosts(fresh);
    };

    int applied = 0;
    int undone = 0;
    int mismatches = 0;
    std::vector<int> entities;
    for (int step = 0; step < 3000; ++step)
    {
        if (unit(rng) < 0.5)
        {
            director.changeVariable(static_cast<int>(rng() % n), static_cast<int>(rng() % n));
            continue;
        }
        entities.clear();
        director.beginMoveEvaluation();
        if (!move.apply(ctx, rng, entities))
        {
            director.endMoveEvaluation(false);
            continue;
        }
        ++applied;
        if (!agrees())
            ++mismatches;
        const bool keep = unit(rng) < 0.5;
        if (!keep)
        {
            director.beginMoveRollback();
            move.undoLast(ctx);
            ++undone;
        }
        director.endMoveEvaluation(keep);
        if (!agrees())
            ++mismatches;
    }
    check(mismatches == 0 && applied > 500 && undone > 150,
          label + (shaped ? " (shaped)" : " (default)") + ": " + std::to_string(applied) + " applied, "
              + std::to_string(undone) + " undone, plugin incremental == a fresh assembly after each");
}

void verifyMacroMoves()
{
    std::printf("== (j) macro moves on the plugin path ==\n");
    for (bool shaped : {false, true})
    {
        EmptyBinMove empty(3);
        walkMacroMove(empty, "EmptyBinMove", shaped);
        RepackMove repack(3, 2);
        walkMacroMove(repack, "RepackMove(3, 2)", shaped);
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
    verifyMacroMoves();
    if (failures == 0)
    {
        std::printf("All bin-packing plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d bin-packing plugin check(s) FAILED.\n", failures);
    return 1;
}

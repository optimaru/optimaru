// Plugin-assembly (ks/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (including undecided items), and every raw total equals
//       ScoreCalculator::rawCosts
//   (b) incremental == full recompute across 3000 random moves driven
//       through the engine's ScoreDirector protocol (plugin path): after
//       each move the score and every constraint's raw total equal the full
//       recompute's, and every constraint is non-zero at some point of the
//       walk
//   (c) partial assemblies: removing one component removes exactly its term
//   (d) weight / level / exponent overrides (globals shape their total; hard
//       rounds once per constraint)
//   (e) constraintTotals() explains exactly the reported score
//   (f) a FULL_ASSERT solve(problem, calculator)
//   (g) the defaultConstraints() builder: named edits == by-hand assembly
//   (h) each component's raw cost, hand-calculated: a positive and a zero
//       case per constraint (raw 0 = no violation)
//   (i) the catalog: info() off the default assembly == the components'
//       kDefaultWeight / kDefaultLevel; the reward guards
//   (j) repeated components and the offset / clip knobs: a hand-calculated
//       repeat, a repeat on its own level (soft next to the hard default),
//       threshold and literal offsets by hand on each global's total (a raw
//       0 counts in literal mode, the reward shifts further), incremental ==
//       a fresh recompute over a random walk and a FULL_ASSERT solve with
//       repeated, shifted components on both levels
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "ks/constraints.hpp"
#include "ks/domain.hpp"
#include "ks/score_calculator.hpp"
#include "ks/solver.hpp"
#include "ks_fixtures.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace ks;
using namespace ks_test;

namespace
{

// Scores `problem`'s current selection with a freshly reset assembly.
Score scoreWith(DecomposedCalculator &calculator, KnapsackProblem &problem)
{
    ScoreCalculator::prepare(problem);
    calculator.resetWorkingSolution(problem);
    return calculator.calculateScore();
}

// One raw total per assembled component, in assembly order.
std::vector<double> rawTotals(const DecomposedCalculator &calculator)
{
    std::vector<double> totals;
    for (const auto &entry : calculator.constraintCosts())
    {
        double sum = 0.0;
        for (double cost : entry.unitCosts)
            sum += cost;
        totals.push_back(sum);
    }
    return totals;
}

// The raw totals the default assembly must report for `raw` (the monolith's
// full recompute), in catalog order; the reward's raw cost is -value.
std::vector<double> expectedTotals(const RawCosts &raw)
{
    return {static_cast<double>(raw.capacityOverflow),  static_cast<double>(raw.resourceOverflow),
            static_cast<double>(raw.missingMandatory),  static_cast<double>(raw.chosenForbidden),
            static_cast<double>(raw.groupViolation),    static_cast<double>(raw.unmetDependencies),
            -static_cast<double>(raw.value)};
}

const char *const kCatalogNames[] = {"knapsackCapacityCost",     "resourceCapacityCost", "mandatoryItemsCost",
                                     "forbiddenItemsCost",       "selectionGroupBoundsCost",
                                     "itemDependencyCost",       "selectedValueCost"};

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    int mismatches = 0;
    int costMismatches = 0;
    const int rounds = 500;
    for (int round = 0; round < rounds; ++round)
    {
        KnapsackProblem p = randomInstance(5 + round % 25, 1000 + round, 0.1);
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        const Score plugin = scoreWith(calculator, p);
        const Score monolithic = ScoreCalculator::calculateScore(p);
        if (!(plugin == monolithic) && ++mismatches <= 3)
        {
            std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n", round,
                        plugin.toString().c_str(), monolithic.toString().c_str());
        }
        if (rawTotals(calculator) != expectedTotals(ScoreCalculator::rawCosts(p)) && ++costMismatches <= 3)
            std::printf("  RAW COST MISMATCH @round %d\n", round);
    }
    check(mismatches == 0, std::to_string(rounds) + " random instances score identically (bit for bit)");
    check(costMismatches == 0,
          std::to_string(rounds) + " random instances: every raw total == ScoreCalculator::rawCosts");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental == full recompute across a random walk, per constraint.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute, per constraint ==\n");
    KnapsackProblem p = randomInstance(40, 7, 0.1);
    ScoreCalculator::prepare(p);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<KnapsackProblem> director(p, calculator);
    KnapsackProblem shadow = p;

    std::mt19937_64 rng(123);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const int n = p.itemCount();
    int mismatches = 0;
    int costMismatches = 0;
    int nonZero[7] = {0, 0, 0, 0, 0, 0, 0};
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.6)
        {
            const int item = static_cast<int>(rng() % n);
            const int value = unit(rng) < 0.15 ? kUndecided : static_cast<int>(rng() % 2);
            director.changeVariable(item, value);
        }
        else
        {
            director.beginVariableChanges();
            const int a = static_cast<int>(rng() % n);
            const int b = static_cast<int>(rng() % n);
            const int va = p.items[a].selected;
            director.changeVariable(a, p.items[b].selected);
            director.changeVariable(b, va);
            director.endVariableChanges();
        }
        for (int i = 0; i < n; ++i)
            shadow.items[i].selected = p.items[i].selected;
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = director.calculateScore();
        if (!(full == incremental) && ++mismatches <= 3)
        {
            std::printf("  MISMATCH @step %d: incremental=%s full=%s\n", step,
                        incremental.toString().c_str(), full.toString().c_str());
        }
        const std::vector<double> expected = expectedTotals(ScoreCalculator::rawCosts(shadow));
        const std::vector<double> running = rawTotals(calculator);
        for (int c = 0; c < 7; ++c)
        {
            if (running[c] != expected[c] && ++costMismatches <= 3)
            {
                std::printf("  RAW COST MISMATCH @step %d %s: incremental=%g full=%g\n", step,
                            kCatalogNames[c], running[c], expected[c]);
            }
            if (expected[c] != 0.0)
                ++nonZero[c];
        }
    }
    check(mismatches == 0, std::to_string(steps) + " random moves, incremental score == full recompute");
    check(costMismatches == 0,
          std::to_string(steps) + " random moves, every constraint's incremental raw total == full recompute");
    for (int c = 0; c < 7; ++c)
    {
        check(nonZero[c] > 0, std::string(kCatalogNames[c]) + " is non-zero on " + std::to_string(nonZero[c])
                                  + " of the walk's states");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Partial assemblies.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Partial assemblies ==\n");
    KnapsackProblem p = extendedInstance();
    select(p, {0});  // five hard terms of 1, value 5
    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        const Score score = scoreWith(calculator, p);
        check(score.hard == -5 && score.soft == 5.0, "full assembly: -5hard / 5soft");
    }
    const char *hardNames[] = {"resourceCapacityCost", "mandatoryItemsCost", "forbiddenItemsCost",
                               "selectionGroupBoundsCost", "itemDependencyCost"};
    for (const char *name : hardNames)
    {
        DecomposedCalculator calculator;
        defaultConstraints().drop(name).into(calculator);
        const Score score = scoreWith(calculator, p);
        check(score.hard == -4 && score.soft == 5.0, std::string("without ") + name + ": exactly its 1 disappears");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().drop("selectedValueCost").into(calculator);
        const Score score = scoreWith(calculator, p);
        check(score.hard == -5 && score.soft == 0.0, "without selectedValueCost: the reward disappears");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level / exponent overrides ==\n");
    // Two resources over by 2 and 3, A + B + C overweight by 5 (cap 10, 15).
    KnapsackProblem p = basicInstance();
    p.resources.push_back({"r1", 1});
    p.resources.push_back({"r2", 0});
    p.items[0].resourceUsage = {{"r1", 3}, {"r2", 1}};
    p.items[1].resourceUsage = {{"r2", 2}};
    p.mandatoryItems = {2, 2, 2};
    select(p, {0, 1});  // weight 10 fits; r1 over 2, r2 over 3; mandatory C x3 missing
    {
        DecomposedCalculator calculator;
        defaultConstraints().soft("resourceCapacityCost").weight("resourceCapacityCost", 3).into(calculator);
        const Score score = scoreWith(calculator, p);
        check(score.hard == -3 && score.soft == 16.0 - 15.0,
              "resourceCapacityCost soft at weight 3: 3 * 5 soft, only mandatory stays hard");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().only({"resourceCapacityCost"}).exponent("resourceCapacityCost", 2).into(calculator);
        const Score score = scoreWith(calculator, p);
        check(score.hard == -25, "resourceCapacityCost exponent 2 shapes the total: (2 + 3)^2 = 25");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().only({"mandatoryItemsCost"}).exponent("mandatoryItemsCost", 2).into(calculator);
        const Score score = scoreWith(calculator, p);
        check(score.hard == -9, "mandatoryItemsCost exponent 2: 3 misses -> 9");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().only({"selectedValueCost"}).exponent("selectedValueCost", 2).into(calculator);
        const Score score = scoreWith(calculator, p);
        check(score.hard == 0 && score.soft == 256.0, "selectedValueCost exponent 2: +16^2 = 256 soft");
    }
    {
        // Weight 0.5 on two hard constraints with 1 each: each rounds on its
        // own (llround(0.5) = 1), so -2, where 0.1.x rounded the sum (-1).
        KnapsackProblem q = extendedInstance();
        select(q, {0});
        DecomposedCalculator calculator;
        defaultConstraints()
            .only({"mandatoryItemsCost", "forbiddenItemsCost"})
            .weight("mandatoryItemsCost", 0.5)
            .weight("forbiddenItemsCost", 0.5)
            .into(calculator);
        const Score score = scoreWith(calculator, q);
        check(score.hard == -2, "two hard constraints at weight 0.5: hard rounds per constraint (-2)");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().soft("knapsackCapacityCost").into(calculator);
        select(p, {0, 1, 2});
        p.mandatoryItems.clear();
        p.resources.clear();
        const Score score = scoreWith(calculator, p);
        check(score.hard == 0 && score.soft == 24.0 - 5.0, "knapsackCapacityCost made soft: 5 over -> 5 soft");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals() explains the score.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    KnapsackProblem p = randomInstance(30, 13);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    const Score score = scoreWith(calculator, p);
    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9 && std::fabs(-soft - score.soft) < 1e-9,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == 7, "one total per assembled component");
    bool oneEach = true;
    for (const auto &entry : calculator.constraintCosts())
        oneEach = oneEach && entry.isGlobal && entry.unitCosts.size() == 1;
    check(oneEach, "constraintCosts(): every component is global with one raw total");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    KnapsackProblem p = randomInstance(30, 11);
    Solver::Config config;
    config.spentLimit = std::chrono::seconds(1);
    config.stepCountLimit = 4000;
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
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
    check(!corrupted, "FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        KnapsackProblem rescored = p;
        check(sameScore(p.score, ScoreCalculator::calculateScore(rescored)),
              "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) The assembly builder.
// -------------------------------------------------------------------------
void verifyAssemblyBuilder()
{
    std::printf("== (g) defaultConstraints() builder ==\n");
    KnapsackProblem p = randomInstance(25, 17);
    DecomposedCalculator byHand;
    {
        byHand.emplace<KnapsackCapacityCost>();
        byHand.emplace<ResourceCapacityCost>(4.0);
        byHand.emplace<MandatoryItemsCost>();
        auto forbidden = std::make_unique<ForbiddenItemsCost>();
        forbidden->setLevel(optimaru::Level::Soft);
        byHand.add(std::move(forbidden));
        byHand.emplace<SelectionGroupBoundsCost>();
        byHand.emplace<SelectedValueCost>();
    }
    const Score expected = scoreWith(byHand, p);
    DecomposedCalculator built;
    defaultConstraints()
        .weight("resourceCapacityCost", 4)
        .soft("forbiddenItemsCost")
        .drop("itemDependencyCost")
        .into(built);
    check(sameScore(scoreWith(built, p), expected), "named edits == the same assembly listed by hand");
    DecomposedCalculator plain;
    defaultConstraints().into(plain);
    check(sameScore(scoreWith(plain, p), ScoreCalculator::calculateScore(p)),
          "defaultConstraints() alone == the monolithic score");
    bool threw = false;
    try
    {
        defaultConstraints().weight("knapsackCapacityCosts", 4);
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    check(threw, "a misspelled constraint name throws instead of doing nothing");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) Raw costs by hand, read through constraintCosts().
// -------------------------------------------------------------------------
std::vector<double> rawCosts(KnapsackProblem &problem, const std::string &name)
{
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    scoreWith(calculator, problem);
    for (const auto &entry : calculator.constraintCosts())
    {
        if (entry.name == name)
            return entry.unitCosts;
    }
    return {};
}

using Costs = std::vector<double>;

void verifyRawCostsByHand()
{
    std::printf("== (h) Raw costs by hand, positive and zero ==\n");
    {
        KnapsackProblem p = basicInstance();
        select(p, {0, 2});  // 11 on 10
        check(rawCosts(p, "knapsackCapacityCost") == Costs{1.0}, "knapsackCapacityCost: [1] (A+C, 11 on 10)");
        check(rawCosts(p, "selectedValueCost") == Costs{-17.0}, "selectedValueCost: [-17] (the reward, A+C)");
        select(p, {0, 1});  // exactly 10
        check(rawCosts(p, "knapsackCapacityCost") == Costs{0.0}, "knapsackCapacityCost: [0] at exactly 10");
        select(p, {});
        check(rawCosts(p, "selectedValueCost") == Costs{0.0}, "selectedValueCost: [0] with nothing selected");
    }
    {
        KnapsackProblem p = extendedInstance();
        select(p, {0});
        check(rawCosts(p, "resourceCapacityCost") == Costs{1.0}, "resourceCapacityCost: [1] (labor 3 on 2)");
        check(rawCosts(p, "mandatoryItemsCost") == Costs{1.0}, "mandatoryItemsCost: [1] (C left out)");
        check(rawCosts(p, "forbiddenItemsCost") == Costs{1.0}, "forbiddenItemsCost: [1] (A selected)");
        check(rawCosts(p, "selectionGroupBoundsCost") == Costs{1.0}, "selectionGroupBoundsCost: [1] (1 of min 2)");
        check(rawCosts(p, "itemDependencyCost") == Costs{1.0}, "itemDependencyCost: [1] (A without B)");
        select(p, {1, 2});
        for (const char *name : {"resourceCapacityCost", "mandatoryItemsCost", "forbiddenItemsCost",
                                 "selectionGroupBoundsCost", "itemDependencyCost", "knapsackCapacityCost"})
        {
            check(rawCosts(p, name) == Costs{0.0}, std::string(name) + ": [0] on {B, C}");
        }
    }
    {
        // Repeated entries count separately; a self-dependency never counts.
        KnapsackProblem p = basicInstance();
        p.capacity = 100;
        p.mandatoryItems = {2, 2};
        p.forbiddenItems = {1, 1, 1};
        p.items[0].requiredItems = {1, 1, 0};
        select(p, {0, 1});
        check(rawCosts(p, "mandatoryItemsCost") == Costs{2.0}, "mandatoryItemsCost: [2] for [C, C]");
        check(rawCosts(p, "forbiddenItemsCost") == Costs{3.0}, "forbiddenItemsCost: [3] for [B, B, B]");
        check(rawCosts(p, "itemDependencyCost") == Costs{0.0}, "itemDependencyCost: [0], B selected, A->A never");
        select(p, {0});
        check(rawCosts(p, "itemDependencyCost") == Costs{2.0}, "itemDependencyCost: [2] for requires [B, B]");
    }
    {
        // Group excess, no maximum = no excess.
        KnapsackProblem p = basicInstance();
        p.selectionGroups.push_back({"g", {0, 1, 2}, 0, 1});
        select(p, {0, 1, 2});
        check(rawCosts(p, "selectionGroupBoundsCost") == Costs{2.0}, "selectionGroupBoundsCost: [2] (3 of max 1)");
        p.selectionGroups[0].maxSelected.reset();
        check(rawCosts(p, "selectionGroupBoundsCost") == Costs{0.0}, "selectionGroupBoundsCost: [0] without a max");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The catalog and the reward guards.
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
bool rowIs(const optimaru::ConstraintInfo &row, const char *name, optimaru::Level level, double weight,
           bool reward = false)
{
    return row.name == name && row.defaultLevel == Component::kDefaultLevel
           && row.defaultWeight == Component::kDefaultWeight && row.defaultLevel == level
           && row.defaultWeight == weight && row.levelFlippable == !reward && row.reward == reward
           && row.isGlobal && row.defaultEnabled;
}

void verifyCatalog()
{
    std::printf("== (i) Catalog and guards ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 7
              && rowIs<KnapsackCapacityCost>(rows[0], "knapsackCapacityCost", Level::Hard, 1.0)
              && rowIs<ResourceCapacityCost>(rows[1], "resourceCapacityCost", Level::Hard, 1.0)
              && rowIs<MandatoryItemsCost>(rows[2], "mandatoryItemsCost", Level::Hard, 1.0)
              && rowIs<ForbiddenItemsCost>(rows[3], "forbiddenItemsCost", Level::Hard, 1.0)
              && rowIs<SelectionGroupBoundsCost>(rows[4], "selectionGroupBoundsCost", Level::Hard, 1.0)
              && rowIs<ItemDependencyCost>(rows[5], "itemDependencyCost", Level::Hard, 1.0)
              && rowIs<SelectedValueCost>(rows[6], "selectedValueCost", Level::Soft, 1.0, /*reward=*/true),
          "info() == the components' constants, in catalog order");
    check(throwsInvalid([] { defaultConstraints().hard("selectedValueCost"); }),
          "hard() on the selectedValueCost reward throws");
    check(throwsInvalid([] { SelectedValueCost().setLevel(Level::Hard); }),
          "SelectedValueCost::setLevel(Hard) throws");
    check(throwsInvalid([] { SelectedValueCost().setLevel(Level::Medium); }),
          "the reward's level is not flippable: setLevel(Medium) throws");
    check(!throwsInvalid([] { defaultConstraints().soft("selectedValueCost"); }),
          "soft() on the reward re-states its level: a no-op");
    check(throwsInvalid([] { defaultConstraints().weight("knapsackCapacityCost", -1); }),
          "a negative weight throws");
    check(throwsInvalid([] { defaultConstraints().exponent("knapsackCapacityCost", 0); }),
          "a zero exponent throws");
    {
        // A medium level needs a domain medium fold: knapsack declares none.
        KnapsackProblem p = basicInstance();
        ScoreCalculator::prepare(p);
        DecomposedCalculator calculator;
        defaultConstraints().level("forbiddenItemsCost", Level::Medium).into(calculator);
        bool threw = false;
        try
        {
            calculator.resetWorkingSolution(p);
        }
        catch (const std::logic_error &)
        {
            threw = true;
        }
        check(threw, "a medium constraint is refused at reset (no medium fold)");
    }
    {
        // The components refuse an unprepared problem.
        KnapsackProblem p = basicInstance();
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        bool threw = false;
        try
        {
            calculator.resetWorkingSolution(p);
        }
        catch (const std::logic_error &)
        {
            threw = true;
        }
        check(threw, "an unprepared problem is refused");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with addInstances(), as the Python bindings
// build it from a pick list. Every knapsack constraint is global, so the
// offset shapes each instance's single total.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string &name)
{
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// The (d) instance: A + B selected (weight 10 fits), resource overflow
// 2 + 3 = 5, mandatory C missing three times, value 9 + 7 = 16; the
// capacity, forbidden, group and dependency totals are 0.
KnapsackProblem shiftedInstance()
{
    KnapsackProblem p = basicInstance();
    p.resources.push_back({"r1", 1});
    p.resources.push_back({"r2", 0});
    p.items[0].resourceUsage = {{"r1", 3}, {"r2", 1}};
    p.items[1].resourceUsage = {{"r2", 2}};
    p.mandatoryItems = {2, 2, 2};
    select(p, {0, 1});
    return p;
}

Score scoreOf(ConstraintAssembly assembly, KnapsackProblem p)
{
    DecomposedCalculator calculator;
    assembly.into(calculator);
    return scoreWith(calculator, p);
}

// Every kind repeated or shifted at least once (threshold, literal on a raw
// 0, negative, a repeated exponent, the reward in both modes, a fractional
// hard weight).
ConstraintAssembly repeatedAssembly()
{
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("resourceCapacityCost", 3.0);
    assembly.offset("selectedValueCost", 40.0).clip("selectedValueCost", false);
    assembly.addInstances(std::move(copyOf("resourceCapacityCost")
                                        .weight("resourceCapacityCost", 0.5)
                                        .exponent("resourceCapacityCost", 2.0)));
    assembly.addInstances(std::move(copyOf("knapsackCapacityCost")
                                        .weight("knapsackCapacityCost", 2.0)
                                        .offset("knapsackCapacityCost", 5.0)
                                        .clip("knapsackCapacityCost", false)));
    assembly.addInstances(std::move(copyOf("mandatoryItemsCost").offset("mandatoryItemsCost", -1.0)));
    assembly.addInstances(std::move(copyOf("forbiddenItemsCost")
                                        .weight("forbiddenItemsCost", 1.5)
                                        .offset("forbiddenItemsCost", -0.5)
                                        .clip("forbiddenItemsCost", false)));
    assembly.addInstances(std::move(copyOf("selectionGroupBoundsCost")
                                        .exponent("selectionGroupBoundsCost", 3.0)
                                        .offset("selectionGroupBoundsCost", 1.0)));
    assembly.addInstances(std::move(copyOf("itemDependencyCost").soft("itemDependencyCost")));
    assembly.addInstances(std::move(copyOf("selectedValueCost")
                                        .weight("selectedValueCost", 0.25)
                                        .exponent("selectedValueCost", 2.0)
                                        .offset("selectedValueCost", 10.0)));
    return assembly;
}

void verifyRepeatsAndOffsets()
{
    std::printf("== (j) Repeated components, offset / clip ==\n");
    {
        // mandatory x 1 + 2 x mandatory^2: 3 + 2 x 9 = 21 hard.
        ConstraintAssembly assembly = copyOf("mandatoryItemsCost");
        assembly.addInstances(std::move(copyOf("mandatoryItemsCost")
                                            .weight("mandatoryItemsCost", 2.0)
                                            .exponent("mandatoryItemsCost", 2.0)));
        KnapsackProblem p = shiftedInstance();
        DecomposedCalculator calculator;
        assembly.into(calculator);
        const Score score = scoreWith(calculator, p);
        check(score.hard == -21 && score.soft == 0.0, "mandatory + 2 x mandatory^2: -(3 + 18) hard");
        const auto entries = calculator.constraintCosts();
        check(entries.size() == 2 && entries[0].unitCosts == entries[1].unitCosts
                  && entries[0].unitCosts == Costs{3.0} && entries[1].weight == 2.0 && entries[1].exponent == 2.0
                  && entries[1].defaultWeight == 1.0 && entries[0].name == entries[1].name,
              "two instances, the same raw total, each with its own knobs");
    }
    {
        // Each instance on its own level: the hard default mandatory misses (3)
        // next to a soft repeat, 2 x (3 - 1) beyond its offset.
        ConstraintAssembly assembly = copyOf("mandatoryItemsCost");
        assembly.addInstances(std::move(copyOf("mandatoryItemsCost")
                                            .soft("mandatoryItemsCost")
                                            .weight("mandatoryItemsCost", 2.0)
                                            .offset("mandatoryItemsCost", 1.0)));
        KnapsackProblem p = shiftedInstance();
        DecomposedCalculator calculator;
        assembly.into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-3, -4.0}),
              "hard mandatory + a soft repeat beyond 1: -3 hard / -4 soft");
        const auto entries = calculator.constraintCosts();
        check(entries.size() == 2 && entries[0].level == optimaru::Level::Hard
                  && entries[1].level == optimaru::Level::Soft && entries[1].defaultLevel == optimaru::Level::Hard,
              "the repeat reports its own level, the default stays hard");
    }
    {
        // Identical picks count twice; the reward too.
        ConstraintAssembly assembly = copyOf("selectedValueCost");
        assembly.addInstances(copyOf("selectedValueCost"));
        check(scoreOf(std::move(assembly), shiftedInstance()).soft == 32.0, "the reward picked twice: +2 x 16");
    }
    {
        const KnapsackProblem p = shiftedInstance();
        check(scoreOf(copyOf("resourceCapacityCost").offset("resourceCapacityCost", 2.0), p).hard == -3,
              "threshold offset 2 on the resource total 5: -3");
        check(scoreOf(copyOf("resourceCapacityCost").offset("resourceCapacityCost", 7.0), p).hard == 0,
              "threshold offset 7 on 5: 0");
        check(scoreOf(copyOf("resourceCapacityCost")
                          .offset("resourceCapacityCost", 7.0)
                          .clip("resourceCapacityCost", false),
                      p)
                      .hard
                  == 2,
              "literal offset 7 on 5: (5 - 7) = -2, a hard reward of 2");
        check(scoreOf(copyOf("resourceCapacityCost")
                          .offset("resourceCapacityCost", 2.0)
                          .exponent("resourceCapacityCost", 3.0),
                      p)
                      .hard
                  == -27,
              "threshold offset 2, exponent 3: (5 - 2)^3 = 27");
        check(scoreOf(copyOf("knapsackCapacityCost")
                          .offset("knapsackCapacityCost", 4.0)
                          .clip("knapsackCapacityCost", false),
                      p)
                      .hard
                  == 4,
              "literal offset 4 on a raw 0 (the selection fits): charged, -4 -> +4 hard");
        check(scoreOf(copyOf("knapsackCapacityCost").offset("knapsackCapacityCost", -1.0), p).hard == 0,
              "a negative threshold offset leaves a raw 0 at 0");
        check(scoreOf(copyOf("knapsackCapacityCost")
                          .offset("knapsackCapacityCost", -1.0)
                          .clip("knapsackCapacityCost", false),
                      p)
                      .hard
                  == -1,
              "a negative literal offset charges a raw 0: 0 + 1");
        check(scoreOf(copyOf("mandatoryItemsCost").offset("mandatoryItemsCost", -1.0), p).hard == -4,
              "a negative threshold offset adds to a non-zero total: 3 + 1");
        check(scoreOf(copyOf("knapsackCapacityCost")
                          .weight("knapsackCapacityCost", 3.0)
                          .offset("knapsackCapacityCost", 0.5)
                          .clip("knapsackCapacityCost", false),
                      p)
                      .hard
                  == 2,
              "a hard literal offset rounds the total: 3 x (0 - 0.5) = -1.5 -> -2, +2 hard");
        check(scoreOf(copyOf("selectedValueCost").offset("selectedValueCost", 10.0), p).soft == 6.0,
              "the reward with a threshold offset 10: only the 16 - 10 beyond it, +6");
        check(scoreOf(copyOf("selectedValueCost")
                          .offset("selectedValueCost", 10.0)
                          .clip("selectedValueCost", false),
                      p)
                      .soft
                  == 26.0,
              "the reward with a literal offset 10: -16 - 10 = -26, +26");
        DecomposedCalculator calculator;
        copyOf("resourceCapacityCost")
            .offset("resourceCapacityCost", 2.5)
            .clip("resourceCapacityCost", false)
            .into(calculator);
        KnapsackProblem q = shiftedInstance();
        scoreWith(calculator, q);
        const auto entries = calculator.constraintCosts();
        check(entries.size() == 1 && entries[0].offset == 2.5 && !entries[0].clip && !entries[0].scoped
                  && entries[0].unitCosts == Costs{5.0},
              "constraintCosts() reports the offset and clip, the raw total unshaped");
    }
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        KnapsackProblem p = randomInstance(40, 7, 0.1);
        ScoreCalculator::prepare(p);
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::ScoreDirector<KnapsackProblem> director(p, calculator);
        std::mt19937_64 rng(321);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const int n = p.itemCount();
        int mismatches = 0;
        double drift = 0.0;
        const int steps = 3000;
        for (int step = 0; step < steps; ++step)
        {
            if (unit(rng) < 0.6)
            {
                const int item = static_cast<int>(rng() % n);
                director.changeVariable(item, unit(rng) < 0.15 ? kUndecided : static_cast<int>(rng() % 2));
            }
            else
            {
                director.beginVariableChanges();
                const int a = static_cast<int>(rng() % n);
                const int b = static_cast<int>(rng() % n);
                const int va = p.items[a].selected;
                director.changeVariable(a, p.items[b].selected);
                director.changeVariable(b, va);
                director.endVariableChanges();
            }
            KnapsackProblem shadow = p;
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            const Score full = scoreWith(fresh, shadow);
            const Score incremental = director.calculateScore();
            drift = std::max(drift, std::fabs(full.soft - incremental.soft));
            if (!optimaru::scoresAgree(incremental, full) && ++mismatches <= 3)
            {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n", step,
                            incremental.toString().c_str(), full.toString().c_str());
            }
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0,
              std::to_string(steps)
                  + " random moves with repeated, shifted components on both levels: incremental == fresh");
    }
    {
        KnapsackProblem p = randomInstance(30, 11);
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(2);
        config.stepCountLimit = 4000;
        config.randomSeed = 5;
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        config.ruinRecreateUnimprovedStepTrigger = 200;
        config.ruinRecreatePolishSteps = 100;
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
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
        check(!corrupted, "FULL_ASSERT solve with repeated, shifted components");
        DecomposedCalculator fresh;
        repeatedAssembly().into(fresh);
        check(!corrupted && optimaru::scoresAgree(p.score, scoreWith(fresh, p)),
              "the reported score == a fresh recompute of the same assembly");
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
    verifyRepeatsAndOffsets();
    if (failures)
    {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}

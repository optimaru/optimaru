// Plugin-assembly tests (mc/constraints.hpp), engine test/ style.
//   (a) monolith vs plugin: the default assembly == ScoreCalculator bit for
//       bit on random assignments (-1 sides included)
//   (b) plugin incremental == full recompute: random moves through the
//       engine's ScoreDirector protocol; the raw costs after the walk equal a
//       fresh reset's
//   (c) dropping one component removes exactly its term
//   (d) weight / level / exponent overrides
//   (e) constraintTotals() explains the score
//   (f) a FULL_ASSERT solve(problem, calculator), exact size included
//   (g) the defaultConstraints() builder: named edits == a by-hand assembly
//   (h) raw costs by hand through constraintCosts(): positive and zero per
//       constraint (fixture H), the edge cases, the -1 rules
//   (i) the catalog (info() == the components' constants) and the guards
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

#include "mc/constraints.hpp"
#include "mc/domain.hpp"
#include "mc/score_calculator.hpp"
#include "mc/solver.hpp"
#include "mc_fixtures.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace mc;
using namespace mc_test;

namespace
{

Score scoreWith(DecomposedCalculator &calculator, MaxCutProblem &problem)
{
    ScoreCalculator::prepare(problem);
    calculator.resetWorkingSolution(problem);
    return calculator.calculateScore();
}

std::vector<std::vector<double>> rawTotals(const DecomposedCalculator &calculator)
{
    std::vector<std::vector<double>> totals;
    for (const auto &entry : calculator.constraintCosts())
        totals.push_back(entry.unitCosts);
    return totals;
}

// -------------------------------------------------------------------------
// (a) Monolith vs plugin.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    int mismatches = 0;
    const int assignments = 500;
    for (int k = 0; k < assignments; ++k)
    {
        MaxCutProblem p = randomInstance(1 + k % 25, 1000 + static_cast<std::uint64_t>(k), k % 3 == 0 ? 0.2 : 0.0);
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        const Score plugin = scoreWith(calculator, p);
        const Score monolith = ScoreCalculator::calculateScore(p);
        if (!sameScore(plugin, monolith) && ++mismatches <= 3)
        {
            std::printf("  MISMATCH #%d: plugin=%s monolith=%s\n", k, plugin.toString().c_str(),
                        monolith.toString().c_str());
        }
    }
    check(mismatches == 0, std::to_string(assignments) + " random assignments (-1 included): plugin == monolith");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Plugin incremental == full recompute.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    int mismatches = 0;
    int rawMismatches = 0;
    int moves = 0;
    for (std::uint64_t seed = 7; seed < 10; ++seed)
    {
        MaxCutProblem p = randomInstance(40, seed, 0.1);
        ScoreCalculator::prepare(p);
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        optimaru::ScoreDirector<MaxCutProblem> director(p, calculator);
        MaxCutProblem shadow = p;

        std::mt19937_64 rng(seed * 123);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const int n = p.vertexCount();
        for (int step = 0; step < 1000; ++step, ++moves)
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
            const Score incremental = director.calculateScore();
            if (!sameScore(full, incremental) && ++mismatches <= 3)
            {
                std::printf("  MISMATCH seed %llu step %d: incremental=%s full=%s\n",
                            static_cast<unsigned long long>(seed), step, incremental.toString().c_str(),
                            full.toString().c_str());
            }
            if (step % 100 == 0)
            {
                DecomposedCalculator fresh;
                addDefaultConstraints(fresh);
                scoreWith(fresh, shadow);
                if (rawTotals(fresh) != rawTotals(calculator))
                    ++rawMismatches;
            }
        }
    }
    check(mismatches == 0, std::to_string(moves) + " random moves, incremental == full recompute");
    check(rawMismatches == 0, "the raw constraint costs during the walk == a fresh reset's");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Dropping one component removes exactly its term.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Partial assemblies ==\n");
    MaxCutProblem p = fixtureH();
    p.side = {0, 1, 1, 1};  // every hard term positive, cut 2, imbalance 2
    ScoreCalculator::prepare(p);
    const RawCosts raw = ScoreCalculator::rawCosts(p);
    struct Case
    {
        const char *name;
        Score expected;
    };
    // The default score is -5hard/2soft; each line is the score without one.
    const Case cases[] = {
        {"fixedPartitionCost", {-5 + raw.fixedTotal(), 2.0}},
        {"samePartitionPairsCost", {-5 + raw.sameTotal(), 2.0}},
        {"differentPartitionPairsCost", {-5 + raw.differentTotal(), 2.0}},
        {"partitionSizeBoundsCost", {-5 + raw.sizeTotal(), 2.0}},
        {"partitionImbalanceCost", {-5, 2.0}},  // weight 0: nothing to remove
        {"cutWeightCost", {-5, 0.0}},
    };
    for (const Case &c : cases)
    {
        DecomposedCalculator calculator;
        defaultConstraints().drop(c.name).into(calculator);
        const Score score = scoreWith(calculator, p);
        check(sameScore(score, c.expected), std::string("without ") + c.name + " -> " + score.toString());
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level / exponent overrides ==\n");
    MaxCutProblem p = fixtureH();
    p.side = {0, 1, 1, 1};
    {
        DecomposedCalculator calculator;
        defaultConstraints().weight("partitionImbalanceCost", 1).into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-5, 0.0}),
              "partitionImbalanceCost at weight 1: soft 2 - 2 = 0");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().weight("cutWeightCost", 3).into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-5, 6.0}), "cutWeightCost at weight 3: soft 6");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().soft("partitionSizeBoundsCost").into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-3, 0.0}),
              "partitionSizeBoundsCost made soft: hard -3, soft 2 - 2 = 0");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().hard("partitionImbalanceCost").weight("partitionImbalanceCost", 1).into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-7, 2.0}), "partitionImbalanceCost made hard: -5 - 2");
    }
    {
        // Exponent 2 shapes each side: the size bounds [1, 1] stay 1 + 1; a
        // side holding two violations becomes 2^2 = 4.
        DecomposedCalculator calculator;
        defaultConstraints().exponent("partitionSizeBoundsCost", 2).into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-5, 2.0}), "size bounds [1, 1] at exponent 2: still 2");
        MaxCutProblem q = fixtureH();
        q.minPartitionSize.reset();
        q.maxPartitionSize.reset();
        q.differentPartitionPairs.clear();
        q.fixedSide = {1, 1, 1, kUnassigned};
        q.side = {0, 0, 0, 1};  // A, B, C off their side 1, all on side 0 (A, B together)
        DecomposedCalculator plain;
        addDefaultConstraints(plain);
        DecomposedCalculator shaped;
        defaultConstraints().exponent("fixedPartitionCost", 2).into(shaped);
        check(scoreWith(plain, q).hard == -3 && scoreWith(shaped, q).hard == -9,
              "fixedPartitionCost [3, 0]: 3 at exponent 1, 9 at exponent 2 (per side)");
    }
    {
        // Exponent on the reward is sign-preserving: -(2^2) = -4, soft +4.
        DecomposedCalculator calculator;
        defaultConstraints().exponent("cutWeightCost", 2).into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-5, 4.0}), "cutWeightCost [-2] at exponent 2: soft 4");
    }
    {
        // Hard is rounded per side: 0.4 on [1, 1] gives round(0.4) * 2 = 0.
        DecomposedCalculator calculator;
        defaultConstraints().weight("partitionSizeBoundsCost", 0.4).into(calculator);
        check(scoreWith(calculator, p).hard == -3, "hard weight 0.4 on size [1, 1]: rounded per side to 0 + 0");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals() explains the score.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() ==\n");
    MaxCutProblem p = randomInstance(30, 41);
    DecomposedCalculator calculator;
    defaultConstraints().weight("partitionImbalanceCost", 2).into(calculator);
    const Score score = scoreWith(calculator, p);
    double hard = 0.0;
    double soft = 0.0;
    std::vector<std::string> names;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
    {
        names.push_back(total.name);
        (total.isHard ? hard : soft) += total.cost;
    }
    check(names == std::vector<std::string>({"fixedPartitionCost", "samePartitionPairsCost",
                                             "differentPartitionPairsCost", "partitionSizeBoundsCost",
                                             "partitionImbalanceCost", "cutWeightCost"}),
          "totals in catalog order");
    check(-hard == static_cast<double>(score.hard) && -soft == score.soft,
          "sum of the totals == the score (" + score.toString() + ")");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
bool fullAssertSolve(MaxCutProblem &p, ConstraintAssembly assembly, const std::string &label)
{
    Solver::Config config;
    config.spentLimit = std::chrono::seconds(10);
    config.stepCountLimit = 4000;
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;
    config.ruinRecreateAsMoveWeight = 0.05;
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    try
    {
        Solver(config).solve(p, calculator);
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        std::printf("  ScoreCorruptionError: %s\n", e.what());
        check(false, label);
        return false;
    }
    check(true, label);
    return true;
}

void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    {
        MaxCutProblem p = randomInstance(30, 11, 0.0, /*bounds=*/false);
        if (fullAssertSolve(p, defaultConstraints(), "FULL_ASSERT plugin solve (flips, swaps, ALNS kicks)"))
        {
            MaxCutProblem rescored = p;
            check(sameScore(p.score, ScoreCalculator::calculateScore(rescored)),
                  "plugin-reported score == monolithic full recompute");
        }
    }
    {
        MaxCutProblem p = randomInstance(30, 12, 0.0, /*bounds=*/false);
        fullAssertSolve(p, defaultConstraints().weight("partitionImbalanceCost", 1).exponent("cutWeightCost", 1.5),
                        "FULL_ASSERT plugin solve with the imbalance on and a shaped cut");
    }
    {
        MaxCutProblem p = randomInstance(30, 13, 0.0, /*bounds=*/false);
        p.minPartitionSize = p.maxPartitionSize = 15;
        int startSideOne = 0;  // the start partition: fixed side, else index % 2
        for (int v = 0; v < p.vertexCount(); ++v)
            startSideOne += (p.fixedSide[v] >= 0 ? p.fixedSide[v] : v % 2) == 1 ? 1 : 0;
        fullAssertSolve(p, defaultConstraints(), "FULL_ASSERT plugin solve in exact-size mode (swaps only)");
        check(p.sideSize(1) == startSideOne && p.sideSize(0) == p.vertexCount() - startSideOne,
              "the exact-size solve kept the start partition's side sizes");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) The assembly builder.
// -------------------------------------------------------------------------
void verifyAssemblyBuilder()
{
    std::printf("== (g) defaultConstraints() builder ==\n");
    MaxCutProblem p = randomInstance(25, 17);
    DecomposedCalculator byHand;
    {
        byHand.emplace<SamePartitionPairsCost>();
        byHand.emplace<DifferentPartitionPairsCost>(4.0);
        auto size = std::make_unique<PartitionSizeBoundsCost>();
        size->setLevel(optimaru::Level::Soft);
        byHand.add(std::move(size));
        byHand.emplace<PartitionImbalanceCost>(1.0);
        byHand.emplace<CutWeightCost>();
    }
    const Score expected = scoreWith(byHand, p);
    DecomposedCalculator built;
    defaultConstraints()
        .weight("differentPartitionPairsCost", 4)
        .soft("partitionSizeBoundsCost")
        .weight("partitionImbalanceCost", 1)
        .drop("fixedPartitionCost")
        .into(built);
    check(sameScore(scoreWith(built, p), expected), "named edits == the same assembly listed by hand");
    DecomposedCalculator plain;
    defaultConstraints().into(plain);
    check(sameScore(scoreWith(plain, p), ScoreCalculator::calculateScore(p)),
          "defaultConstraints() alone == the monolithic score");
    bool threw = false;
    try
    {
        defaultConstraints().weight("cutWeightCosts", 4);
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
using Costs = std::vector<double>;

Costs rawCosts(MaxCutProblem &problem, const std::string &name)
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

void verifyRawCostsByHand()
{
    std::printf("== (h) Raw costs by hand, positive and zero ==\n");
    {
        MaxCutProblem p = fixtureH();
        p.side = {0, 1, 1, 1};
        check(rawCosts(p, "fixedPartitionCost") == Costs{1.0, 0.0}, "fixedPartitionCost: [1, 0] (A off side 1)");
        check(rawCosts(p, "samePartitionPairsCost") == Costs{1.0, 0.0},
              "samePartitionPairsCost: [1, 0] ((A, B) split, A on side 0)");
        check(rawCosts(p, "differentPartitionPairsCost") == Costs{0.0, 1.0},
              "differentPartitionPairsCost: [0, 1] (B, C on side 1)");
        check(rawCosts(p, "partitionSizeBoundsCost") == Costs{1.0, 1.0},
              "partitionSizeBoundsCost: [1, 1] (1 and 3 for [2, 2])");
        check(rawCosts(p, "partitionImbalanceCost") == Costs{2.0}, "partitionImbalanceCost: [2]");
        check(rawCosts(p, "cutWeightCost") == Costs{-2.0}, "cutWeightCost: [-2] (the reward, A-B cut)");
        p.side = {1, 1, 0, 0};
        for (const char *name : {"fixedPartitionCost", "samePartitionPairsCost", "differentPartitionPairsCost",
                                 "partitionSizeBoundsCost"})
        {
            check(rawCosts(p, name) == Costs({0.0, 0.0}), std::string(name) + ": [0, 0] on (A, B | C, D)");
        }
        check(rawCosts(p, "partitionImbalanceCost") == Costs{0.0}, "partitionImbalanceCost: [0] on 2 + 2");
        check(rawCosts(p, "cutWeightCost") == Costs{-3.0}, "cutWeightCost: [-3] (B-C cut)");
        p.side = {1, 1, 1, 1};
        const Costs cut = rawCosts(p, "cutWeightCost");
        check(cut == Costs{0.0} && !std::signbit(cut[0]), "cutWeightCost: [0.0] (not -0.0) with nothing cut");
    }
    {
        // A self-loop is never cut; parallel edges add up.
        MaxCutProblem p = graph({"A", "B"}, {{0, 0, 5}, {0, 1, 1}, {1, 0, 2}});
        p.side = {0, 1};
        check(rawCosts(p, "cutWeightCost") == Costs{-3.0}, "cutWeightCost: [-3] (self-loop 5 never cut)");
    }
    {
        // Self-pairs.
        MaxCutProblem p = graph({"A", "B"}, {});
        p.samePartitionPairs = {{0, 0}};
        p.differentPartitionPairs = {{1, 1}};
        p.side = {0, 1};
        check(rawCosts(p, "samePartitionPairsCost") == Costs({0.0, 0.0}), "a same self-pair: [0, 0]");
        check(rawCosts(p, "differentPartitionPairsCost") == Costs({0.0, 1.0}), "a different self-pair: [0, 1]");
    }
    {
        // The -1 rules, the same as the monolith's (mc/domain.hpp).
        MaxCutProblem p = graph({"A", "B", "C"}, {{0, 1, 4}, {1, 2, 5}});
        p.fixedSide[2] = 1;
        p.samePartitionPairs = {{0, 1}, {1, 0}};
        p.differentPartitionPairs = {{0, 2}};
        p.side = {0, kUnassigned, kUnassigned};
        check(rawCosts(p, "samePartitionPairsCost") == Costs({1.0, 0.0}),
              "-1: (A, B) split while B is unassigned; (B, A) charges nothing");
        check(rawCosts(p, "differentPartitionPairsCost") == Costs({0.0, 0.0}), "-1: never joined");
        check(rawCosts(p, "fixedPartitionCost") == Costs({0.0, 0.0}), "-1: an unassigned fixed vertex costs 0");
        check(rawCosts(p, "cutWeightCost") == Costs{0.0}, "-1: no edge cut");
        check(rawCosts(p, "partitionImbalanceCost") == Costs{1.0}, "-1: the imbalance counts assigned vertices");
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
bool rowIs(const optimaru::ConstraintInfo &row,
           const char *name,
           optimaru::Level level,
           double weight,
           bool isGlobal,
           bool reward = false)
{
    return row.name == name && row.defaultLevel == Component::kDefaultLevel
           && row.defaultWeight == Component::kDefaultWeight && row.defaultLevel == level
           && row.defaultWeight == weight && row.levelFlippable == !reward && row.reward == reward
           && row.isGlobal == isGlobal && row.defaultEnabled == (weight > 0.0);
}

void verifyCatalog()
{
    std::printf("== (i) Catalog and guards ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 6
              && rowIs<FixedPartitionCost>(rows[0], "fixedPartitionCost", Level::Hard, 1.0, false)
              && rowIs<SamePartitionPairsCost>(rows[1], "samePartitionPairsCost", Level::Hard, 1.0, false)
              && rowIs<DifferentPartitionPairsCost>(rows[2], "differentPartitionPairsCost", Level::Hard, 1.0, false)
              && rowIs<PartitionSizeBoundsCost>(rows[3], "partitionSizeBoundsCost", Level::Hard, 1.0, false)
              && rowIs<PartitionImbalanceCost>(rows[4], "partitionImbalanceCost", Level::Soft, 0.0, true)
              && rowIs<CutWeightCost>(rows[5], "cutWeightCost", Level::Soft, 1.0, true, /*reward=*/true),
          "info() == the components' constants, in catalog order");
    check(!rows[4].defaultEnabled, "partitionImbalanceCost is opt-in: default_enabled false at weight 0");
    check(defaultConstraints().names()
              == std::vector<std::string>({"fixedPartitionCost", "samePartitionPairsCost",
                                           "differentPartitionPairsCost", "partitionSizeBoundsCost",
                                           "partitionImbalanceCost", "cutWeightCost"}),
          "names() in catalog order");
    check(throwsInvalid([] { defaultConstraints().hard("cutWeightCost"); }), "hard() on the cutWeightCost reward throws");
    check(throwsInvalid([] { CutWeightCost().setLevel(Level::Hard); }), "CutWeightCost::setLevel(Hard) throws");
    check(throwsInvalid([] { CutWeightCost().setLevel(Level::Medium); }),
          "the reward's level is not flippable: setLevel(Medium) throws");
    check(!throwsInvalid([] { defaultConstraints().soft("cutWeightCost"); }),
          "soft() on the reward re-states its level: a no-op");
    check(throwsInvalid([] { defaultConstraints().weight("fixedPartitionCost", -1); }), "a negative weight throws");
    check(throwsInvalid([] { defaultConstraints().exponent("fixedPartitionCost", 0); }), "a zero exponent throws");
    {
        // A medium level needs a domain medium fold: max-cut declares none.
        MaxCutProblem p = fixtureH();
        p.side = {1, 1, 0, 0};
        ScoreCalculator::prepare(p);
        DecomposedCalculator calculator;
        defaultConstraints().level("partitionSizeBoundsCost", Level::Medium).into(calculator);
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
        MaxCutProblem p = fixtureH();
        p.side = {1, 1, 0, 0};
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
    if (failures)
    {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}

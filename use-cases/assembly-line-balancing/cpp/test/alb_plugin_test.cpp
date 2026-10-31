// Assembly-line-balancing plugin-assembly tests (alb/constraints.hpp):
//   (a) the default assembly reproduces the monolith bit for bit — the score
//       and every constraint's raw total — on 500 random assignments
//   (b) plugin incremental == full recompute over random move sequences
//   (c) dropping one component drops exactly that term
//   (d) weight / level / exponent overrides, hand-calculated
//   (e) constraintTotals() explains the score
//   (f) a FULL_ASSERT plugin solve
//   (g) the name-based assembly builder equals a hand-built assembly
//   (h) raw costs by hand per constraint (positive and zero) through
//       constraintCosts(), unit granularity included
//   (i) the catalog: names, order, levels, weights, flippability
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "alb/constraints.hpp"
#include "alb/domain.hpp"
#include "alb/score_calculator.hpp"
#include "alb/solver.hpp"
#include "alb_fixtures.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace alb;
using namespace alb_test;

namespace
{

const std::vector<std::string> kNames = {
    "stationCapacityCost",       "taskPrecedenceCost",    "maxStationsCost",
    "taskStationBoundsCost",     "fixedTaskStationsCost", "forbiddenTaskStationsCost",
    "sameStationGroupsCost",     "differentStationPairsCost", "usedStationsCost",
    "workloadImbalanceCost",
};

std::map<std::string, double> monolithTotals(AssemblyLineBalancingProblem &p)
{
    const RawCosts raw = ScoreCalculator::rawCosts(p);
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

std::map<std::string, double> pluginTotals(const DecomposedCalculator &calculator)
{
    std::map<std::string, double> totals;
    for (const auto &entry : calculator.constraintCosts())
    {
        double sum = 0.0;
        for (double cost : entry.unitCosts)
            sum += cost;
        totals[entry.name] = sum;
    }
    return totals;
}

std::vector<double> unitCosts(AssemblyLineBalancingProblem p, const std::string &name)
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

Score scoreWith(ConstraintAssembly assembly, AssemblyLineBalancingProblem p)
{
    ScoreCalculator::prepare(p);
    DecomposedCalculator calculator;
    assembly.into(calculator);
    calculator.resetWorkingSolution(p);
    return calculator.calculateScore();
}

// -------------------------------------------------------------------------
// (a) Default assembly == monolith.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolith ==\n");
    int scoreMismatches = 0;
    int totalMismatches = 0;
    std::mt19937_64 rng(2024);
    for (int round = 0; round < 500; ++round)
    {
        AssemblyLineBalancingProblem p = randomInstance(3 + round % 12, 1000 + round);
        ScoreCalculator::prepare(p);
        // Stations anywhere in [-1, V - 1].
        for (Task &t : p.tasks)
            t.station = static_cast<int>(rng() % (p.stationCount + 1)) - 1;
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        calculator.resetWorkingSolution(p);
        const Score plugin = calculator.calculateScore();
        AssemblyLineBalancingProblem shadow = p;
        const Score monolith = ScoreCalculator::calculateScore(shadow);
        if (!sameScore(plugin, monolith) && ++scoreMismatches <= 3)
            std::printf("  SCORE MISMATCH round %d: plugin=%s monolith=%s\n", round,
                        plugin.toString().c_str(), monolith.toString().c_str());
        if (pluginTotals(calculator) != monolithTotals(shadow) && ++totalMismatches <= 3)
            std::printf("  TOTALS MISMATCH round %d\n", round);
        if (calculator.constraintCosts().size() != kNames.size())
            ++totalMismatches;
    }
    check(scoreMismatches == 0, "500 random assignments: plugin score == monolith score");
    check(totalMismatches == 0, "500 random assignments: every raw total == ScoreCalculator::rawCosts");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Plugin incremental == full recompute.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    int mismatches = 0;
    for (std::uint64_t seed = 1; seed <= 6; ++seed)
    {
        AssemblyLineBalancingProblem p = randomInstance(12 + static_cast<int>(seed), seed);
        ScoreCalculator::prepare(p);
        DecomposedCalculator calculator;
        // Weighted workload, a shaped unit and a shaped global: every
        // accumulation path is exercised.
        defaultConstraints()
            .weight("workloadImbalanceCost", 2)
            .exponent("stationCapacityCost", 2.0)
            .exponent("sameStationGroupsCost", 2.0)
            .into(calculator);
        optimaru::ScoreDirector<AssemblyLineBalancingProblem> director(p, calculator);
        std::mt19937_64 rng(500 + seed);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const int n = p.taskCount();
        const int values = p.stationCount;
        for (int step = 0; step < 500; ++step)
        {
            const double kind = unit(rng);
            if (kind < 0.55)
            {
                const int t = static_cast<int>(rng() % n);
                director.changeVariable(t, unit(rng) < 0.1 ? -1 : static_cast<int>(rng() % values));
            }
            else if (kind < 0.85)
            {
                const int a = static_cast<int>(rng() % n);
                const int b = static_cast<int>(rng() % n);
                const int va = p.tasks[a].station;
                const int vb = p.tasks[b].station;
                director.beginVariableChanges();
                director.changeVariable(a, vb);
                director.changeVariable(b, va);
                director.endVariableChanges();
            }
            else
            {
                const int victim = static_cast<int>(rng() % values);
                director.beginVariableChanges();
                for (int t = 0; t < n; ++t)
                {
                    if (p.tasks[t].station == victim)
                        director.changeVariable(t, static_cast<int>(rng() % values));
                }
                director.endVariableChanges();
            }
            const Score incremental = director.calculateScore();
            AssemblyLineBalancingProblem shadow = p;
            DecomposedCalculator fresh;
            defaultConstraints()
                .weight("workloadImbalanceCost", 2)
                .exponent("stationCapacityCost", 2.0)
                .exponent("sameStationGroupsCost", 2.0)
                .into(fresh);
            fresh.resetWorkingSolution(shadow);
            const Score full = fresh.calculateScore();
            if (!sameScore(incremental, full) && ++mismatches <= 3)
                std::printf("  MISMATCH seed %llu step %d: incremental=%s full=%s\n",
                            static_cast<unsigned long long>(seed), step,
                            incremental.toString().c_str(), full.toString().c_str());
        }
    }
    check(mismatches == 0, "3000 random moves on shaped / re-weighted assemblies, incremental == reset");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Dropping one component drops exactly its term (fixture H).
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Partial assemblies ==\n");
    const std::map<std::string, std::int64_t> hardRaw = {
        {"stationCapacityCost", 1},   {"taskPrecedenceCost", 1},    {"maxStationsCost", 1},
        {"taskStationBoundsCost", 2}, {"fixedTaskStationsCost", 1}, {"forbiddenTaskStationsCost", 1},
        {"sameStationGroupsCost", 1}, {"differentStationPairsCost", 2},
    };
    bool ok = true;
    for (const auto &[name, raw] : hardRaw)
    {
        const Score dropped = scoreWith(defaultConstraints().drop(name), fixtureH());
        ok &= sameScore(dropped, Score{-10 + raw, -3.0});
    }
    check(ok, "dropping each hard component removes exactly its raw cost from -10hard");
    check(sameScore(scoreWith(defaultConstraints().drop("usedStationsCost"), fixtureH()), Score{-10, 0.0}),
          "dropping usedStationsCost removes the -3soft");
    check(sameScore(scoreWith(defaultConstraints().only({"usedStationsCost"}), fixtureH()), Score{0, -3.0}),
          "only usedStationsCost: 0hard/-3soft");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Overrides, hand-calculated on fixture H.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Overrides ==\n");
    check(sameScore(scoreWith(defaultConstraints().weight("workloadImbalanceCost", 2), fixtureH()),
                    Score{-10, -9.0}),
          "workload weight 2: -3 - 2 * 3 = -9soft");
    check(sameScore(scoreWith(defaultConstraints().soft("differentStationPairsCost"), fixtureH()),
                    Score{-8, -5.0}),
          "pairs soft: -8hard / -5soft");
    check(sameScore(scoreWith(defaultConstraints().hard("usedStationsCost"), fixtureH()), Score{-13, 0.0}),
          "used stations hard: -13hard / 0soft");
    // Per-station exponent: pairs are 1 on station 1 and 1 on station 0 (the
    // self-pair) -> 1^2 + 1^2 = 2, unchanged; bounds are 1 on station 0 and 1
    // on station 3 -> unchanged too. maxStations x10 -> 10.
    check(sameScore(scoreWith(defaultConstraints().weight("maxStationsCost", 10), fixtureH()),
                    Score{-19, -3.0}),
          "max stations weight 10: -19hard");
    // A global's exponent shapes its total: used stations 3^2 = 9.
    check(sameScore(scoreWith(defaultConstraints().exponent("usedStationsCost", 2.0), fixtureH()),
                    Score{-10, -9.0}),
          "used stations exponent 2 shapes the total: 3^2 = 9");
    // A unit's exponent shapes each station: H with D also on station 1 puts
    // A(3) + C(2) + D(6) = 11 on station 1 -> (11 - 5)^2 = 36.
    AssemblyLineBalancingProblem heavy = fixtureH();
    heavy.tasks[3].station = 1;
    const Score shaped = scoreWith(defaultConstraints().exponent("stationCapacityCost", 2.0), heavy);
    const Score plain = scoreWith(defaultConstraints(), heavy);
    check(shaped.hard - plain.hard == -(36 - 6), "capacity exponent 2 per station: 6 -> 36");
    // Fractional hard weights round per unit (half away from zero).
    check(sameScore(scoreWith(defaultConstraints().only({"differentStationPairsCost"})
                                  .weight("differentStationPairsCost", 0.5),
                              fixtureH()),
                    Score{-2, 0.0}),
          "pairs weight 0.5: two stations each round 0.5 -> 1");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals explains the score.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals ==\n");
    AssemblyLineBalancingProblem p = fixtureH();
    ScoreCalculator::prepare(p);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(p);
    const Score score = calculator.calculateScore();
    double hard = 0.0;
    double soft = 0.0;
    for (const auto &total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
    check(score.hard == -static_cast<std::int64_t>(hard) && score.soft == -soft,
          "summed totals == the score");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT plugin solve.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    AssemblyLineBalancingProblem p = randomInstance(14, 91);
    p.precedences.clear();
    for (int t = 1; t < p.taskCount(); t += 2)
        p.precedences.push_back({t - 1, t});
    Solver::Config config;
    config.spentLimit = std::chrono::seconds(30);
    config.stepCountLimit = 1500;
    config.randomSeed = 4;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    config.ruinRecreateUnimprovedStepTrigger = 100;
    config.ruinRecreatePolishSteps = 50;
    DecomposedCalculator calculator;
    defaultConstraints().weight("workloadImbalanceCost", 1).into(calculator);
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
    DecomposedCalculator fresh;
    defaultConstraints().weight("workloadImbalanceCost", 1).into(fresh);
    fresh.resetWorkingSolution(p);
    check(sameScore(p.score, fresh.calculateScore()), "reported score == the assembly's full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) The builder equals a hand-built assembly.
// -------------------------------------------------------------------------
void verifyAssemblyBuilder()
{
    std::printf("== (g) Assembly builder ==\n");
    ConstraintAssembly byHand;
    byHand.emplace<StationCapacityCost>();
    byHand.emplace<TaskPrecedenceCost>();
    auto bounds = std::make_unique<TaskStationBoundsCost>(3.0);
    bounds->level = optimaru::Level::Soft;
    byHand.add(std::move(bounds));
    byHand.emplace<UsedStationsCost>();
    ConstraintAssembly named = defaultConstraints()
                                   .only({"stationCapacityCost", "taskPrecedenceCost",
                                          "taskStationBoundsCost", "usedStationsCost"})
                                   .weight("taskStationBoundsCost", 3)
                                   .soft("taskStationBoundsCost");
    for (std::uint64_t seed = 0; seed < 20; ++seed)
    {
        AssemblyLineBalancingProblem p = randomInstance(8, 300 + seed);
        ConstraintAssembly a;
        a.emplace<StationCapacityCost>();
        a.emplace<TaskPrecedenceCost>();
        auto b = std::make_unique<TaskStationBoundsCost>(3.0);
        b->level = optimaru::Level::Soft;
        a.add(std::move(b));
        a.emplace<UsedStationsCost>();
        ConstraintAssembly c = defaultConstraints()
                                   .only({"stationCapacityCost", "taskPrecedenceCost",
                                          "taskStationBoundsCost", "usedStationsCost"})
                                   .weight("taskStationBoundsCost", 3)
                                   .soft("taskStationBoundsCost");
        if (!sameScore(scoreWith(std::move(a), p), scoreWith(std::move(c), p)))
        {
            check(false, "builder == hand-built on random instance " + std::to_string(seed));
            return;
        }
    }
    check(byHand.names() == named.names(), "the builder keeps the same components");
    check(true, "builder == hand-built on 20 random instances");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) Raw costs by hand through constraintCosts().
// -------------------------------------------------------------------------
void verifyRawCostsByHand()
{
    std::printf("== (h) Raw costs by hand ==\n");
    using V = std::vector<double>;
    // H: V = 4 stations; loads 3 / 5 / 0 / 6.
    check(unitCosts(fixtureH(), "stationCapacityCost") == V({0, 0, 0, 1}), "capacity per station: [0, 0, 0, 1]");
    check(unitCosts(fixtureH(), "taskPrecedenceCost") == V({1}), "precedence (global): [1]");
    check(unitCosts(fixtureH(), "maxStationsCost") == V({0, 0, 0, 1}), "max stations per station: [0, 0, 0, 1]");
    check(unitCosts(fixtureH(), "taskStationBoundsCost") == V({1, 0, 0, 1}), "bounds per station: [1, 0, 0, 1]");
    check(unitCosts(fixtureH(), "fixedTaskStationsCost") == V({0, 1, 0, 0}), "fixed per station: [0, 1, 0, 0]");
    check(unitCosts(fixtureH(), "forbiddenTaskStationsCost") == V({0, 1, 0, 0}),
          "forbidden per station: [0, 1, 0, 0]");
    check(unitCosts(fixtureH(), "sameStationGroupsCost") == V({1}), "same-station groups (global): [1]");
    check(unitCosts(fixtureH(), "differentStationPairsCost") == V({1, 1, 0, 0}),
          "pairs per station: self-pair on 0, (A, C) on 1");
    check(unitCosts(fixtureH(), "usedStationsCost") == V({3}), "used stations (global): [3]");
    check(unitCosts(fixtureH(), "workloadImbalanceCost") == V({3}), "workload (global): [3]");

    for (const std::string &name : kNames)
    {
        const V costs = unitCosts(fixtureZ(), name);
        double sum = 0.0;
        for (double cost : costs)
            sum += cost;
        const double expected = name == "usedStationsCost" ? 2.0 : 0.0;
        check(sum == expected, name + " on fixture Z: " + (expected == 0.0 ? "0" : "2 stations"));
    }
    // The 0.1.5 test fixture (test_zoning_and_station_domain_costs_match_manual_values).
    AssemblyLineBalancingProblem zoning;
    zoning.cycleTime = 5;
    zoning.maxStations = 1;
    zoning.tasks = {task("A", 3, 1, 0, 1, 0), task("B", 3, 0, 0, std::nullopt, std::nullopt, {0})};
    zoning.precedences = {{0, 1}};
    zoning.sameStationGroups = {{"zone", {0, 1}}};
    zoning.differentStationPairs = {{0, 1}};
    check(unitCosts(zoning, "maxStationsCost") == V({0, 1}), "zoning: max stations [0, 1]");
    check(unitCosts(zoning, "fixedTaskStationsCost") == V({0, 1}), "zoning: fixed [0, 1]");
    check(unitCosts(zoning, "forbiddenTaskStationsCost") == V({1, 0}), "zoning: forbidden [1, 0]");
    check(unitCosts(zoning, "sameStationGroupsCost") == V({1}), "zoning: same-station groups [1]");
    check(unitCosts(zoning, "differentStationPairsCost") == V({0, 0}), "zoning: pairs [0, 0]");
    check(unitCosts(zoning, "taskPrecedenceCost") == V({1}), "zoning: precedence [1]");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The catalog.
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
           bool isGlobal)
{
    return row.name == name && row.defaultLevel == Component::kDefaultLevel
           && row.defaultWeight == Component::kDefaultWeight && row.defaultLevel == level
           && row.defaultWeight == weight && row.levelFlippable && !row.reward && row.isGlobal == isGlobal
           && row.defaultEnabled == (weight > 0.0);
}

void verifyCatalog()
{
    std::printf("== (i) Catalog ==\n");
    check(catalogNames() == kNames, "catalog order: the ten registry names, 0.1.5 catalog order");
    std::vector<std::string> sortedAssembly = defaultConstraints().names();
    std::vector<std::string> sortedCatalog = catalogNames();
    std::sort(sortedAssembly.begin(), sortedAssembly.end());
    std::sort(sortedCatalog.begin(), sortedCatalog.end());
    check(sortedAssembly == sortedCatalog, "catalogNames() is a permutation of the default assembly");
    const auto rows = catalogRows();
    using L = optimaru::Level;
    check(rows.size() == 10 && rowIs<StationCapacityCost>(rows[0], "stationCapacityCost", L::Hard, 1, false)
              && rowIs<TaskPrecedenceCost>(rows[1], "taskPrecedenceCost", L::Hard, 1, true)
              && rowIs<MaxStationsCost>(rows[2], "maxStationsCost", L::Hard, 1, false)
              && rowIs<TaskStationBoundsCost>(rows[3], "taskStationBoundsCost", L::Hard, 1, false)
              && rowIs<FixedTaskStationsCost>(rows[4], "fixedTaskStationsCost", L::Hard, 1, false)
              && rowIs<ForbiddenTaskStationsCost>(rows[5], "forbiddenTaskStationsCost", L::Hard, 1, false)
              && rowIs<SameStationGroupsCost>(rows[6], "sameStationGroupsCost", L::Hard, 1, true)
              && rowIs<DifferentStationPairsCost>(rows[7], "differentStationPairsCost", L::Hard, 1, false)
              && rowIs<UsedStationsCost>(rows[8], "usedStationsCost", L::Soft, 1, true)
              && rowIs<WorkloadImbalanceCost>(rows[9], "workloadImbalanceCost", L::Soft, 0, true),
          "catalogRows(): eight hard 1, used stations soft 1, workload soft 0 (opt-in); all flippable");
    check(!throwsInvalid([] { defaultConstraints().soft("stationCapacityCost"); })
              && !throwsInvalid([] { defaultConstraints().hard("workloadImbalanceCost"); }),
          "every level flips both ways");
    check(throwsInvalid([] { defaultConstraints().weight("usedStationsCost", -1.0); }),
          "a negative weight is refused");
    check(throwsInvalid([] { defaultConstraints().exponent("usedStationsCost", 0.0); }),
          "a non-positive exponent is refused");
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

    if (failures == 0)
    {
        std::printf("All assembly-line-balancing plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d assembly-line-balancing plugin check(s) FAILED.\n", failures);
    return 1;
}

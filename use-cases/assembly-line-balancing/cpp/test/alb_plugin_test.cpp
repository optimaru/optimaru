// Assembly-line-balancing plugin-assembly tests (alb/constraints.hpp):
//   (a) the default assembly reproduces the monolith bit for bit — the score
//       and every constraint's raw total — on 500 random assignments
//   (b) plugin incremental == full recompute over random move sequences:
//       the score and every constraint's raw total after each move, each
//       constraint non-zero at least once
//   (c) dropping one component drops exactly that term
//   (d) weight / level / exponent overrides, hand-calculated
//   (e) constraint_totals() explains the score
//   (f) a ScoreChecks::Full plugin solve
//   (g) the name-based assembly builder equals a hand-built assembly
//   (h) raw costs by hand per constraint (positive and zero) through
//       constraint_costs(), unit granularity included
//   (i) the catalog: names, order, levels, weights, flippability
//   (j) repeated components and the offset / clip knobs: hand-calculated
//       repeats of a per-station and a global rule, threshold and literal
//       offsets by hand (literal mode charges every evaluated station, an
//       empty one included; the scoped maxStationsCost and
//       forbiddenTaskStationsCost charge only their scoped stations; a global
//       shapes its one total), the literal formula refused on hard at reset
//       and kept on soft, incremental == a fresh recompute over random move
//       sequences and a ScoreChecks::Full solve with repeated, shifted components
#include <algorithm>
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
#include "optimaru/model/editor.hpp"

using namespace alb;
using namespace alb_test;

namespace
{

std::map<std::string, double> monolithTotals(AssemblyLineBalancingProblem &p)
{
    return namedTotals(ScoreCalculator::rawCosts(p));
}

std::map<std::string, double> pluginTotals(const DecomposedCalculator &calculator)
{
    std::map<std::string, double> totals;
    for (const auto &entry : calculator.constraint_costs())
    {
        double sum = 0.0;
        for (double cost : entry.unit_costs)
            sum += cost;
        totals[entry.name] = sum;
    }
    return totals;
}

std::vector<double> unit_costs(AssemblyLineBalancingProblem p, const std::string &name)
{
    ScoreCalculator::prepare(p);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(p);
    calculator.score();
    for (const auto &entry : calculator.constraint_costs())
    {
        if (entry.name == name)
            return entry.unit_costs;
    }
    return {};
}

Score scoreWith(ConstraintAssembly assembly, AssemblyLineBalancingProblem p)
{
    ScoreCalculator::prepare(p);
    DecomposedCalculator calculator;
    assembly.into(calculator);
    calculator.rebuild(p);
    return calculator.score();
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
        calculator.rebuild(p);
        const Score plugin = calculator.score();
        AssemblyLineBalancingProblem shadow = p;
        const Score monolith = ScoreCalculator::score(shadow);
        if (!sameScore(plugin, monolith) && ++scoreMismatches <= 3)
            std::printf("  SCORE MISMATCH round %d: plugin=%s monolith=%s\n", round,
                        plugin.str().c_str(), monolith.str().c_str());
        if (pluginTotals(calculator) != monolithTotals(shadow) && ++totalMismatches <= 3)
            std::printf("  TOTALS MISMATCH round %d\n", round);
        if (calculator.constraint_costs().size() != kNames.size())
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
    int totalMismatches = 0;
    int printBudget = 3;
    std::map<std::string, int> nonZeroStates;
    for (std::uint64_t seed = 1; seed <= 6; ++seed)
    {
        AssemblyLineBalancingProblem p = randomInstance(12 + static_cast<int>(seed), seed);
        ScoreCalculator::prepare(p);
        DecomposedCalculator calculator;
        // Weighted workload (the opt-in weight-0 constraint switched on), a
        // shaped unit and a shaped global: every accumulation path is
        // exercised.
        defaultConstraints()
            .weight("workloadImbalanceCost", 2)
            .exponent("stationCapacityCost", 2.0)
            .exponent("sameStationGroupsCost", 2.0)
            .into(calculator);
        optimaru::Editor<AssemblyLineBalancingProblem> editor(p, calculator);
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
                editor.assign(t, unit(rng) < 0.1 ? -1 : static_cast<int>(rng() % values));
            }
            else if (kind < 0.85)
            {
                const int a = static_cast<int>(rng() % n);
                const int b = static_cast<int>(rng() % n);
                const int va = p.tasks[a].station;
                const int vb = p.tasks[b].station;
                editor.begin_batch();
                editor.assign(a, vb);
                editor.assign(b, va);
                editor.end_batch();
            }
            else
            {
                const int victim = static_cast<int>(rng() % values);
                editor.begin_batch();
                for (int t = 0; t < n; ++t)
                {
                    if (p.tasks[t].station == victim)
                        editor.assign(t, static_cast<int>(rng() % values));
                }
                editor.end_batch();
            }
            const Score incremental = editor.score();
            AssemblyLineBalancingProblem shadow = p;
            DecomposedCalculator fresh;
            defaultConstraints()
                .weight("workloadImbalanceCost", 2)
                .exponent("stationCapacityCost", 2.0)
                .exponent("sameStationGroupsCost", 2.0)
                .into(fresh);
            fresh.rebuild(shadow);
            const Score full = fresh.score();
            if (!sameScore(incremental, full) && ++mismatches <= 3)
                std::printf("  MISMATCH seed %llu step %d: incremental=%s full=%s\n",
                            static_cast<unsigned long long>(seed), step,
                            incremental.str().c_str(), full.str().c_str());
            const std::string where = "seed " + std::to_string(seed) + " step " + std::to_string(step);
            if (!sameTotals(pluginTotals(calculator), pluginTotals(fresh), nonZeroStates, where, printBudget))
                ++totalMismatches;
        }
    }
    check(mismatches == 0, "3000 random moves on shaped / re-weighted assemblies, incremental == reset");
    check(totalMismatches == 0, "after every move each constraint's raw total == the reset's");
    check(everyConstraintNonZero(nonZeroStates, kNames), "every constraint is non-zero at least once in the walk");
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
// (e) constraint_totals explains the score.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals ==\n");
    AssemblyLineBalancingProblem p = fixtureH();
    ScoreCalculator::prepare(p);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(p);
    const Score score = calculator.score();
    double hard = 0.0;
    double soft = 0.0;
    for (const auto &total : calculator.constraint_totals())
        (total.is_hard ? hard : soft) += total.cost;
    check(score.hard == -static_cast<std::int64_t>(hard) && score.soft == -soft,
          "summed totals == the score");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) ScoreChecks::Full plugin solve.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    AssemblyLineBalancingProblem p = randomInstance(14, 91);
    p.precedences.clear();
    for (int t = 1; t < p.taskCount(); t += 2)
        p.precedences.push_back({t - 1, t});
    Solver::Config config;
    config.time_limit = std::chrono::seconds(30);
    config.step_limit = 1500;
    config.seed = 4;
    config.score_checks = optimaru::ScoreChecks::Full;
    config.ruinRecreateUnimprovedStepTrigger = 100;
    config.ruinRecreatePolishSteps = 50;
    DecomposedCalculator calculator;
    defaultConstraints().weight("workloadImbalanceCost", 1).into(calculator);
    bool corrupted = false;
    try
    {
        Solver(config).solve(p, calculator);
    }
    catch (const optimaru::ScoreMismatchError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "FULL_ASSERT plugin solve without score corruption");
    DecomposedCalculator fresh;
    defaultConstraints().weight("workloadImbalanceCost", 1).into(fresh);
    fresh.rebuild(p);
    check(sameScore(p.score, fresh.score()), "reported score == the assembly's full recompute");
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
// (h) Raw costs by hand through constraint_costs().
// -------------------------------------------------------------------------
void verifyRawCostsByHand()
{
    std::printf("== (h) Raw costs by hand ==\n");
    using V = std::vector<double>;
    // H: V = 4 stations; loads 3 / 5 / 0 / 6.
    check(unit_costs(fixtureH(), "stationCapacityCost") == V({0, 0, 0, 1}), "capacity per station: [0, 0, 0, 1]");
    check(unit_costs(fixtureH(), "taskPrecedenceCost") == V({1}), "precedence (global): [1]");
    check(unit_costs(fixtureH(), "maxStationsCost") == V({0, 0, 0, 1}), "max stations per station: [0, 0, 0, 1]");
    check(unit_costs(fixtureH(), "taskStationBoundsCost") == V({1, 0, 0, 1}), "bounds per station: [1, 0, 0, 1]");
    check(unit_costs(fixtureH(), "fixedTaskStationsCost") == V({0, 1, 0, 0}), "fixed per station: [0, 1, 0, 0]");
    check(unit_costs(fixtureH(), "forbiddenTaskStationsCost") == V({0, 1, 0, 0}),
          "forbidden per station: [0, 1, 0, 0]");
    check(unit_costs(fixtureH(), "sameStationGroupsCost") == V({1}), "same-station groups (global): [1]");
    check(unit_costs(fixtureH(), "differentStationPairsCost") == V({1, 1, 0, 0}),
          "pairs per station: self-pair on 0, (A, C) on 1");
    check(unit_costs(fixtureH(), "usedStationsCost") == V({3}), "used stations (global): [3]");
    check(unit_costs(fixtureH(), "workloadImbalanceCost") == V({3}), "workload (global): [3]");

    for (const std::string &name : kNames)
    {
        const V costs = unit_costs(fixtureZ(), name);
        double sum = 0.0;
        for (double cost : costs)
            sum += cost;
        const double expected = name == "usedStationsCost" ? 2.0 : 0.0;
        check(sum == expected, name + " on fixture Z: " + (expected == 0.0 ? "0" : "2 stations"));
    }
    // The zoning fixture (as in test_zoning_and_station_domain_costs_match_manual_values).
    AssemblyLineBalancingProblem zoning;
    zoning.cycleTime = 5;
    zoning.maxStations = 1;
    zoning.tasks = {task("A", 3, 1, 0, 1, 0), task("B", 3, 0, 0, std::nullopt, std::nullopt, {0})};
    zoning.precedences = {{0, 1}};
    zoning.sameStationGroups = {{"zone", {0, 1}}};
    zoning.differentStationPairs = {{0, 1}};
    check(unit_costs(zoning, "maxStationsCost") == V({0, 1}), "zoning: max stations [0, 1]");
    check(unit_costs(zoning, "fixedTaskStationsCost") == V({0, 1}), "zoning: fixed [0, 1]");
    check(unit_costs(zoning, "forbiddenTaskStationsCost") == V({1, 0}), "zoning: forbidden [1, 0]");
    check(unit_costs(zoning, "sameStationGroupsCost") == V({1}), "zoning: same-station groups [1]");
    check(unit_costs(zoning, "differentStationPairsCost") == V({0, 0}), "zoning: pairs [0, 0]");
    check(unit_costs(zoning, "taskPrecedenceCost") == V({1}), "zoning: precedence [1]");
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
           bool is_global)
{
    return row.name == name && row.default_level == Component::kDefaultLevel
           && row.default_weight == Component::kDefaultWeight && row.default_level == level
           && row.default_weight == weight && row.level_flippable && !row.reward && row.is_global == is_global
           && row.default_enabled == (weight > 0.0);
}

void verifyCatalog()
{
    std::printf("== (i) Catalog ==\n");
    check(catalogNames() == kNames, "catalog order: the ten registry names in catalog order");
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

// -------------------------------------------------------------------------
// (j) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with add_instances(), as the Python bindings
// build it from a pick list.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string &name)
{
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// The defaults (workload switched on) with two of them shifted by a
// threshold, plus repeats of the scoped per-task rules (one literal, one a
// fractional threshold), the pairwise delta rule (negative literal), the
// no-delta capacity rule and the four globals (precedence literal), and
// repeats on the other level than their default: the literal copies of hard
// rules on soft (hard refuses the literal formula with an offset), among
// them the station bounds and the scoped forbidden stations (fractional
// weight), and the used stations promoted to hard (threshold).
ConstraintAssembly repeatedAssembly()
{
    ConstraintAssembly assembly = defaultConstraints();
    assembly.weight("workloadImbalanceCost", 1.0);
    assembly.offset("stationCapacityCost", 1.0);
    assembly.offset("taskPrecedenceCost", 1.0);
    assembly.add_instances(std::move(copyOf("taskPrecedenceCost")
                                        .soft("taskPrecedenceCost")
                                        .offset("taskPrecedenceCost", 1.0)
                                        .clip("taskPrecedenceCost", false)));
    assembly.add_instances(std::move(copyOf("maxStationsCost")
                                        .soft("maxStationsCost")
                                        .weight("maxStationsCost", 2.0)
                                        .offset("maxStationsCost", 1.0)
                                        .clip("maxStationsCost", false)));
    assembly.add_instances(std::move(copyOf("forbiddenTaskStationsCost")
                                        .weight("forbiddenTaskStationsCost", 3.0)
                                        .offset("forbiddenTaskStationsCost", 0.5)));
    assembly.add_instances(std::move(copyOf("differentStationPairsCost")
                                        .soft("differentStationPairsCost")
                                        .offset("differentStationPairsCost", -1.0)
                                        .clip("differentStationPairsCost", false)));
    assembly.add_instances(std::move(copyOf("stationCapacityCost").exponent("stationCapacityCost", 2.0)));
    assembly.add_instances(std::move(
        copyOf("sameStationGroupsCost").exponent("sameStationGroupsCost", 2.0).offset("sameStationGroupsCost", 1.0)));
    assembly.add_instances(std::move(copyOf("usedStationsCost")
                                        .weight("usedStationsCost", 0.25)
                                        .exponent("usedStationsCost", 2.0)
                                        .offset("usedStationsCost", 2.0)));
    assembly.add_instances(std::move(copyOf("workloadImbalanceCost")
                                        .weight("workloadImbalanceCost", 0.5)
                                        .offset("workloadImbalanceCost", 3.0)
                                        .clip("workloadImbalanceCost", false)));
    assembly.add_instances(std::move(copyOf("taskStationBoundsCost")));
    assembly.add_instances(std::move(copyOf("taskStationBoundsCost")
                                        .soft("taskStationBoundsCost")
                                        .weight("taskStationBoundsCost", 1.5)
                                        .offset("taskStationBoundsCost", 1.0)
                                        .clip("taskStationBoundsCost", false)));
    assembly.add_instances(std::move(copyOf("forbiddenTaskStationsCost")
                                        .soft("forbiddenTaskStationsCost")
                                        .offset("forbiddenTaskStationsCost", 1.0)
                                        .clip("forbiddenTaskStationsCost", false)));
    assembly.add_instances(std::move(copyOf("usedStationsCost")
                                        .hard("usedStationsCost")
                                        .weight("usedStationsCost", 2.0)
                                        .offset("usedStationsCost", 3.0)));
    return assembly;
}

void verifyRepeatsAndOffsets()
{
    std::printf("== (j) Repeated components, offset / clip ==\n");
    {
        // Fixture H uses 3 stations: 3 + 10 x (3 - 2).
        ConstraintAssembly assembly = copyOf("usedStationsCost");
        assembly.add_instances(
            std::move(copyOf("usedStationsCost").weight("usedStationsCost", 10.0).offset("usedStationsCost", 2.0)));
        AssemblyLineBalancingProblem p = fixtureH();
        ScoreCalculator::prepare(p);
        DecomposedCalculator calculator;
        assembly.into(calculator);
        calculator.rebuild(p);
        check(sameScore(calculator.score(), Score{0, -13.0}), "used stations + 10 x (used - 2): -13 soft");
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].unit_costs == entries[1].unit_costs && entries[1].weight == 10.0
                  && entries[1].offset == 2.0 && entries[0].offset == 0.0 && entries[1].default_weight == 1.0,
              "two global instances, the same total, each with its own knobs");
    }
    {
        // Station bounds [1, 0, 0, 1] + 2 x their square per station.
        ConstraintAssembly assembly = copyOf("taskStationBoundsCost");
        assembly.add_instances(std::move(
            copyOf("taskStationBoundsCost").weight("taskStationBoundsCost", 2.0).exponent("taskStationBoundsCost", 2.0)));
        check(sameScore(scoreWith(std::move(assembly), fixtureH()), Score{-6, 0.0}),
              "station bounds + 2 x bounds^2 per station: -6 hard");
    }
    {
        // Each instance on its own level: the soft default used stations (3)
        // next to a hard repeat, 2 x (3 - 1) beyond its offset.
        ConstraintAssembly assembly = copyOf("usedStationsCost");
        assembly.add_instances(std::move(copyOf("usedStationsCost")
                                            .hard("usedStationsCost")
                                            .weight("usedStationsCost", 2.0)
                                            .offset("usedStationsCost", 1.0)));
        AssemblyLineBalancingProblem p = fixtureH();
        ScoreCalculator::prepare(p);
        DecomposedCalculator calculator;
        assembly.into(calculator);
        calculator.rebuild(p);
        check(sameScore(calculator.score(), Score{-4, -3.0}),
              "soft used stations + a hard repeat beyond 1: -4 hard / -3 soft");
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].level == optimaru::Level::Soft
                  && entries[1].level == optimaru::Level::Hard && entries[1].default_level == optimaru::Level::Soft,
              "the repeat reports its own level, the default stays soft");
        // The scoped forbidden stations, hard by default, with a soft literal
        // repeat: 2 x (x - 2) on its stations 0 and 1 only: 2 x (-2 - 1).
        ConstraintAssembly scoped = copyOf("forbiddenTaskStationsCost");
        scoped.add_instances(std::move(copyOf("forbiddenTaskStationsCost")
                                          .soft("forbiddenTaskStationsCost")
                                          .weight("forbiddenTaskStationsCost", 2.0)
                                          .offset("forbiddenTaskStationsCost", 2.0)
                                          .clip("forbiddenTaskStationsCost", false)));
        check(sameScore(scoreWith(std::move(scoped), fixtureH()), Score{-1, 6.0}),
              "hard forbidden stations + a soft literal repeat on its two stations: -1 hard / +6 soft");
    }
    {
        // Fixture H, V = 4: capacity [0, 0, 0, 1] at weight 3, offset 0.5.
        // The hard threshold rounds per station: 1.5 -> 2 on station 3, the
        // others stay 0. The literal formula is refused on hard, so it is
        // checked on soft, unrounded: 1.5 on station 3, -1.5 on the others.
        check(sameScore(scoreWith(copyOf("stationCapacityCost")
                                      .weight("stationCapacityCost", 3.0)
                                      .offset("stationCapacityCost", 0.5),
                                  fixtureH()),
                        Score{-2, 0.0}),
              "hard threshold offset 0.5 at weight 3, rounded per station: -2 hard");
        check(sameScore(scoreWith(copyOf("stationCapacityCost")
                                      .soft("stationCapacityCost")
                                      .weight("stationCapacityCost", 3.0)
                                      .offset("stationCapacityCost", 0.5)
                                      .clip("stationCapacityCost", false),
                                  fixtureH()),
                        Score{0, 3.0}),
              "soft literal offset 0.5 at weight 3: 1.5 - 3 x 1.5 -> +3 soft");
        check(sameScore(scoreWith(copyOf("stationCapacityCost").offset("stationCapacityCost", -1.0), fixtureH()),
                        Score{-2, 0.0}),
              "threshold offset -1 adds to the non-zero station only");
        check(sameScore(scoreWith(copyOf("stationCapacityCost")
                                      .soft("stationCapacityCost")
                                      .offset("stationCapacityCost", -1.0)
                                      .clip("stationCapacityCost", false),
                                  fixtureH()),
                        Score{0, -5.0}),
              "soft literal offset -1 adds to every station: 3 x 1 + 2");
        // Scoped: maxStationsCost evaluates station 3 only (raw 1),
        // forbiddenTaskStationsCost stations 0 and 1 (raw 0, 1).
        check(sameScore(scoreWith(copyOf("maxStationsCost")
                                      .soft("maxStationsCost")
                                      .offset("maxStationsCost", 2.0)
                                      .clip("maxStationsCost", false),
                                  fixtureH()),
                        Score{0, 1.0}),
              "soft max stations literal offset 2: (1 - 2) on its one station");
        check(sameScore(scoreWith(copyOf("forbiddenTaskStationsCost")
                                      .soft("forbiddenTaskStationsCost")
                                      .offset("forbiddenTaskStationsCost", 2.0)
                                      .clip("forbiddenTaskStationsCost", false),
                                  fixtureH()),
                        Score{0, 3.0}),
              "soft forbidden stations literal offset 2: (0 - 2) + (1 - 2) on its two stations");
        check(sameScore(scoreWith(copyOf("usedStationsCost").offset("usedStationsCost", 5.0).clip("usedStationsCost", false),
                                  fixtureH()),
                        Score{0, 2.0}),
              "used stations literal offset 5: 3 - 5, a reward");
    }
    {
        // Fixture Z: every hard term 0. The scoped components report their
        // units, and a literal offset charges -offset on exactly those.
        AssemblyLineBalancingProblem p = fixtureZ();
        ScoreCalculator::prepare(p);
        DecomposedCalculator calculator;
        defaultConstraints().into(calculator);
        calculator.rebuild(p);
        calculator.score();
        bool unitsOk = true;
        for (const auto &entry : calculator.constraint_costs())
        {
            if (entry.name == "maxStationsCost")
                unitsOk = unitsOk && entry.scoped && entry.units == std::vector<int>{3};
            else if (entry.name == "forbiddenTaskStationsCost")
                unitsOk = unitsOk && entry.scoped && entry.units == std::vector<int>({0, 1});
            else
                unitsOk = unitsOk && !entry.scoped;
        }
        check(p.stationCount == 4 && unitsOk, "scoped units: max stations [3], forbidden stations [0, 1]");
        check(sameScore(scoreWith(copyOf("maxStationsCost")
                                      .soft("maxStationsCost")
                                      .offset("maxStationsCost", 5.0)
                                      .clip("maxStationsCost", false),
                                  fixtureZ()),
                        Score{0, 5.0}),
              "an all-zero max stations, soft literal offset 5: one evaluated station, +5");
        check(sameScore(scoreWith(copyOf("forbiddenTaskStationsCost")
                                      .soft("forbiddenTaskStationsCost")
                                      .offset("forbiddenTaskStationsCost", 5.0)
                                      .clip("forbiddenTaskStationsCost", false),
                                  fixtureZ()),
                        Score{0, 10.0}),
              "an all-zero forbidden stations, soft literal offset 5: two evaluated stations, +10");
    }
    {
        // On hard the literal formula with an offset is refused at reset,
        // whichever sign and setter order, for a hard default (per station,
        // scoped and global) and for the soft used stations moved to hard:
        // on fixture Z (every hard term 0) +5 would reward the scoped
        // stations (+5 hard, cancelling another violation), -5 would charge
        // stations that hold no violation.
        auto refusal = [](const std::string &name) {
            return "Constraint '" + name
                   + "' cannot use clip=false with a non-zero offset on a level that counts "
                     "toward the hard score (hard 0 must mean no violation)";
        };
        auto resetRefusal = [](ConstraintAssembly assembly) {
            try
            {
                scoreWith(std::move(assembly), fixtureZ());
            }
            catch (const std::logic_error &error)
            {
                return std::string(error.what());
            }
            return std::string();
        };
        check(resetRefusal(copyOf("stationCapacityCost")
                               .offset("stationCapacityCost", 5.0)
                               .clip("stationCapacityCost", false))
                      == refusal("stationCapacityCost")
                  && resetRefusal(copyOf("stationCapacityCost")
                                      .clip("stationCapacityCost", false)
                                      .offset("stationCapacityCost", -5.0))
                         == refusal("stationCapacityCost"),
              "station capacity literal offset +-5 on hard: refused at reset");
        check(resetRefusal(copyOf("maxStationsCost").offset("maxStationsCost", 5.0).clip("maxStationsCost", false))
                  == refusal("maxStationsCost"),
              "scoped max stations literal offset on hard: refused at reset");
        check(resetRefusal(copyOf("taskPrecedenceCost")
                               .offset("taskPrecedenceCost", 1.0)
                               .clip("taskPrecedenceCost", false))
                  == refusal("taskPrecedenceCost"),
              "task precedence (a global) literal offset on hard: refused at reset");
        check(resetRefusal(copyOf("usedStationsCost")
                               .offset("usedStationsCost", 5.0)
                               .clip("usedStationsCost", false)
                               .hard("usedStationsCost"))
                  == refusal("usedStationsCost"),
              "used stations literal offset moved to hard afterwards: refused at reset");
        ConstraintAssembly repeated = copyOf("stationCapacityCost");
        repeated.add_instances(std::move(copyOf("stationCapacityCost")
                                            .offset("stationCapacityCost", 1.0)
                                            .clip("stationCapacityCost", false)));
        check(resetRefusal(std::move(repeated)) == refusal("stationCapacityCost"),
              "a hard literal repeat next to the default: refused at reset");
        check(resetRefusal(copyOf("stationCapacityCost").offset("stationCapacityCost", 5.0)).empty()
                  && resetRefusal(copyOf("stationCapacityCost").clip("stationCapacityCost", false)).empty()
                  && resetRefusal(copyOf("stationCapacityCost")
                                      .offset("stationCapacityCost", 5.0)
                                      .clip("stationCapacityCost", false)
                                      .soft("stationCapacityCost"))
                         .empty(),
              "station capacity: a threshold offset or clip=false with offset 0 on hard, and the "
              "literal offset on soft, are allowed");
    }
    {
        // Random walks: incremental == a fresh calculator of the same assembly.
        int mismatches = 0;
        double drift = 0.0;
        for (std::uint64_t seed = 1; seed <= 6; ++seed)
        {
            AssemblyLineBalancingProblem p = randomInstance(12 + static_cast<int>(seed), seed);
            ScoreCalculator::prepare(p);
            DecomposedCalculator calculator;
            repeatedAssembly().into(calculator);
            optimaru::Editor<AssemblyLineBalancingProblem> editor(p, calculator);
            std::mt19937_64 rng(900 + seed);
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            const int n = p.taskCount();
            const int values = p.stationCount;
            for (int step = 0; step < 500; ++step)
            {
                const double kind = unit(rng);
                if (kind < 0.55)
                {
                    const int t = static_cast<int>(rng() % n);
                    editor.assign(t, unit(rng) < 0.1 ? -1 : static_cast<int>(rng() % values));
                }
                else if (kind < 0.85)
                {
                    const int a = static_cast<int>(rng() % n);
                    const int b = static_cast<int>(rng() % n);
                    const int va = p.tasks[a].station;
                    const int vb = p.tasks[b].station;
                    editor.begin_batch();
                    editor.assign(a, vb);
                    editor.assign(b, va);
                    editor.end_batch();
                }
                else
                {
                    const int victim = static_cast<int>(rng() % values);
                    editor.begin_batch();
                    for (int t = 0; t < n; ++t)
                    {
                        if (p.tasks[t].station == victim)
                            editor.assign(t, static_cast<int>(rng() % values));
                    }
                    editor.end_batch();
                }
                const Score incremental = editor.score();
                AssemblyLineBalancingProblem shadow = p;
                DecomposedCalculator fresh;
                repeatedAssembly().into(fresh);
                fresh.rebuild(shadow);
                const Score full = fresh.score();
                drift = std::max(drift, std::fabs(full.soft - incremental.soft));
                // The engine's ScoreChecks::Full rule (scores_agree): hard exact,
                // soft within 1e-6 (the fractional weights accumulate ulps).
                if (!optimaru::scores_agree(incremental, full) && ++mismatches <= 3)
                    std::printf("  MISMATCH seed %llu step %d: incremental=%s full=%s\n",
                                static_cast<unsigned long long>(seed), step, incremental.str().c_str(),
                                full.str().c_str());
            }
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0,
              "3000 random moves with repeated, shifted components on both levels: incremental == fresh");
    }
    {
        AssemblyLineBalancingProblem p = randomInstance(14, 91);
        p.precedences.clear();
        for (int t = 1; t < p.taskCount(); t += 2)
            p.precedences.push_back({t - 1, t});
        Solver::Config config;
        config.time_limit = std::chrono::seconds(30);
        config.step_limit = 1500;
        config.seed = 4;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruinRecreateUnimprovedStepTrigger = 100;
        config.ruinRecreatePolishSteps = 50;
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        bool corrupted = false;
        try
        {
            Solver(config).solve(p, calculator);
        }
        catch (const optimaru::ScoreMismatchError &e)
        {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted, "FULL_ASSERT solve with repeated, shifted components");
        DecomposedCalculator fresh;
        repeatedAssembly().into(fresh);
        fresh.rebuild(p);
        check(!corrupted && optimaru::scores_agree(p.score, fresh.score()),
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

    if (failures == 0)
    {
        std::printf("All assembly-line-balancing plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d assembly-line-balancing plugin check(s) FAILED.\n", failures);
    return 1;
}

// Plugin-assembly (ts/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (unassigned included)
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: each component in isolation, hand-computed
//   (d) weight / level / exponent overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(schedule, calculator) under FULL_ASSERT
//   (g) raw costs by hand through constraintCosts(): one positive and one
//       zero case per constraint (unweighted, before exponent and level)
//   (h) the catalog (defaultConstraints().info()) == the components'
//       kDefaultWeight / kDefaultLevel
//   (i) the medium level (assignmentCountImbalanceCost): default fold,
//       hard() / soft() / an explicit weight, and incremental == full
//       recompute under each override
//   (j) repeated components and the offset / clip knobs: a hand-calculated
//       repeat, threshold and literal offsets by hand (the scoped
//       teamUnavailabilityCost evaluates only the teams with a fact), the
//       globals' totals shaped once, incremental == a fresh recompute over a
//       random walk and a FULL_ASSERT solve with repeated, shifted components
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/api/score/director/score_director.hpp"
#include "ts/constraints.hpp"
#include "ts/domain.hpp"
#include "ts/score_calculator.hpp"
#include "ts/solver.hpp"

using namespace ts;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

void checkScore(const std::string &label,
                const Score &actual,
                long expectedHard,
                double expectedSoft)
{
    const bool ok = actual.hard == expectedHard
                    && std::fabs(actual.soft - expectedSoft) < 1e-9;
    std::printf("  [%s] %-46s got=%s expected=%ldhard/%gsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedSoft);
    if (!ok)
        ++failures;
}

bool sameScore(const Score &a, const Score &b)
{
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

TournamentSchedule makeBase(int teamCount, int dayCount)
{
    TournamentSchedule s;
    for (int t = 0; t < teamCount; ++t)
        s.teams.push_back({"T" + std::to_string(t)});
    for (int d = 0; d < dayCount; ++d)
        s.days.push_back({d});
    return s;
}

void addAssignment(TournamentSchedule &s, int day, int team = -1)
{
    TeamAssignment assignment;
    assignment.id = static_cast<std::int64_t>(s.teamAssignments.size());
    assignment.day = day;
    assignment.indexInDay = 0;
    assignment.team = team;
    s.teamAssignments.push_back(assignment);
}

TournamentSchedule
buildRandomInstance(int teamCount, int dayCount, int perDay, std::uint64_t seed)
{
    TournamentSchedule s = makeBase(teamCount, dayCount);
    std::mt19937_64 rng(seed);
    std::int64_t id = 0;
    for (int d = 0; d < dayCount; ++d)
    {
        for (int i = 0; i < perDay; ++i)
        {
            TeamAssignment assignment;
            assignment.id = id++;
            assignment.day = d;
            assignment.indexInDay = i;
            s.teamAssignments.push_back(assignment);
        }
    }
    const int penaltyCount = teamCount * dayCount / 8;
    while (static_cast<int>(s.unavailabilityPenalties.size()) < penaltyCount)
    {
        const int team = static_cast<int>(rng() % teamCount);
        const int day = static_cast<int>(rng() % dayCount);
        bool duplicate = false;
        for (const UnavailabilityPenalty &penalty : s.unavailabilityPenalties)
        {
            if (penalty.team == team && penalty.day == day)
                duplicate = true;
        }
        if (!duplicate)
            s.unavailabilityPenalties.push_back({team, day});
    }
    return s;
}

Score scoreWith(DecomposedCalculator &calculator, TournamentSchedule &s)
{
    calculator.resetWorkingSolution(s);
    return calculator.calculateScore();
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    TournamentSchedule s = buildRandomInstance(/*teams*/ 5,
                                               /*days*/ 6,
                                               /*perDay*/ 3,
                                               /*seed*/ 7);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int mismatches = 0;
    const int rounds = 500;
    for (int round = 0; round < rounds; ++round)
    {
        for (TeamAssignment &assignment : s.teamAssignments)
        {
            assignment.team = unit(rng) < 0.10
                                  ? -1
                                  : static_cast<int>(rng() % s.teamCount());
        }
        const Score plugin = scoreWith(calculator, s);
        const Score monolithic = ScoreCalculator::calculateScore(s);
        if (!sameScore(plugin, monolithic))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n",
                            round,
                            plugin.toString().c_str(),
                            monolithic.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(rounds) + " random assignments score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    TournamentSchedule s = buildRandomInstance(/*teams*/ 5,
                                               /*days*/ 6,
                                               /*perDay*/ 3,
                                               /*seed*/ 7);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<TournamentSchedule> director(s, calculator);
    TournamentSchedule shadow = s;

    std::mt19937_64 rng(123);
    const int assignmentCount = static_cast<int>(s.teamAssignments.size());
    const int valueCount = s.teamCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            const int assignment = static_cast<int>(rng() % assignmentCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(assignment, value);
        }
        else
        {
            const int a = static_cast<int>(rng() % assignmentCount);
            const int b = static_cast<int>(rng() % assignmentCount);
            const int va = s.teamAssignments[a].team;
            const int vb = s.teamAssignments[b].team;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        for (int i = 0; i < assignmentCount; ++i)
            shadow.teamAssignments[i].team = s.teamAssignments[i].team;
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = director.calculateScore();
        if (!sameScore(full, incremental))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n",
                            step,
                            incremental.toString().c_str(),
                            full.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps)
              + " random moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Single-component assemblies on hand-computed instances.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Single-component assemblies ==\n");

    {  // teamConflictCost alone: three assignments of one team on
        // one day -> C(3, 2) = 3 pairs.
        TournamentSchedule s = makeBase(/*teams*/ 2, /*days*/ 2);
        addAssignment(s, 0, 0);
        addAssignment(s, 0, 0);
        addAssignment(s, 0, 0);
        DecomposedCalculator calculator;
        calculator.emplace<TeamConflictCost>();
        checkScore("teamConflictCost alone",
                   scoreWith(calculator, s),
                   -3,
                   0.0);
    }
    {  // teamUnavailabilityCost alone: one fact, one hit — and a second
        // assignment in the same cell still costs one (per fact, not per
        // assignment).
        TournamentSchedule s = makeBase(/*teams*/ 2, /*days*/ 2);
        s.unavailabilityPenalties.push_back({0, 1});
        addAssignment(s, 1, 0);
        addAssignment(s, 1, 0);
        DecomposedCalculator calculator;
        calculator.emplace<TeamUnavailabilityCost>();
        checkScore("teamUnavailabilityCost alone (per fact)",
                   scoreWith(calculator, s),
                   -1,
                   0.0);
    }
    {  // The same fact with the team playing elsewhere: silent.
        TournamentSchedule s = makeBase(/*teams*/ 2, /*days*/ 2);
        s.unavailabilityPenalties.push_back({0, 1});
        addAssignment(s, 0, 0);
        DecomposedCalculator calculator;
        calculator.emplace<TeamUnavailabilityCost>();
        checkScore("teamUnavailabilityCost silent off the day",
                   scoreWith(calculator, s),
                   0,
                   0.0);
    }
    {  // assignmentCountImbalanceCost alone: loads 2 and 0 -> only the loaded
        // team enters the balance, so the unfairness is 0.
        TournamentSchedule s = makeBase(/*teams*/ 2, /*days*/ 2);
        addAssignment(s, 0, 0);
        addAssignment(s, 1, 0);
        DecomposedCalculator calculator;
        calculator.emplace<AssignmentCountImbalanceCost>();
        checkScore("assignmentCountImbalanceCost, single loaded team",
                   scoreWith(calculator, s),
                   0,
                   0.0);
    }
    {  // Loads 2 and 1 -> sqrt(5 - 9/2) = sqrt(0.5), folded at kMediumScale.
        TournamentSchedule s = makeBase(/*teams*/ 2, /*days*/ 3);
        addAssignment(s, 0, 0);
        addAssignment(s, 1, 0);
        addAssignment(s, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<AssignmentCountImbalanceCost>();
        const double expected
            = -ScoreCalculator::unfairnessFromAggregates(3, 5, 2)
              * kMediumScale;
        checkScore("assignmentCountImbalanceCost, loads 2 and 1",
                   scoreWith(calculator, s),
                   0,
                   expected);
    }
    {  // confrontationCountImbalanceCost alone: one confronting pair -> a
        // single loaded item, so the unfairness is 0 (and the medium level is
        // not in this assembly at all).
        TournamentSchedule s = makeBase(/*teams*/ 2, /*days*/ 1);
        addAssignment(s, 0, 0);
        addAssignment(s, 0, 1);
        DecomposedCalculator calculator;
        calculator.emplace<ConfrontationCountImbalanceCost>();
        checkScore("confrontationCountImbalanceCost, single pair",
                   scoreWith(calculator, s),
                   0,
                   0.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    TournamentSchedule s = makeBase(/*teams*/ 2, /*days*/ 2);
    addAssignment(s, 0, 0);
    addAssignment(s, 0, 0);
    addAssignment(s, 0, 0);

    {
        DecomposedCalculator calculator;
        calculator.emplace<TeamConflictCost>(10.0);
        checkScore("teamConflictCost re-weighted to 10",
                   scoreWith(calculator, s),
                   -30,
                   0.0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<TeamConflictCost>();
        component->setLevel(optimaru::Level::Soft);
        calculator.add(std::move(component));
        checkScore("teamConflictCost demoted to soft",
                   scoreWith(calculator, s),
                   0,
                   -3.0);
    }
    {
        // exponent 2 shapes the raw pair count (3) before the weight.
        DecomposedCalculator calculator;
        auto component = std::make_unique<TeamConflictCost>();
        component->setExponent(2.0);
        calculator.add(std::move(component));
        checkScore("teamConflictCost with exponent 2",
                   scoreWith(calculator, s),
                   -9,
                   0.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    TournamentSchedule s = buildRandomInstance(/*teams*/ 5,
                                               /*days*/ 6,
                                               /*perDay*/ 3,
                                               /*seed*/ 13);
    std::mt19937_64 rng(5);
    for (TeamAssignment &assignment : s.teamAssignments)
        assignment.team = static_cast<int>(rng() % s.teamCount());

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    const Score score = scoreWith(calculator, s);

    // A medium total is in medium units: fold it like the calculator does.
    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
    {
        if (total.level == optimaru::Level::Hard)
            hard += total.cost;
        else if (total.level == optimaru::Level::Medium)
            soft += total.cost * kMediumFold.unit;
        else
            soft += total.cost;
    }
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-6,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == 4,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    TournamentSchedule s = buildRandomInstance(/*teams*/ 6,
                                               /*days*/ 6,
                                               /*perDay*/ 3,
                                               /*seed*/ 11);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    bool corrupted = false;
    try
    {
        Solver solver(config);
        solver.solve(s, calculator);
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(s);
        check(sameScore(s.score, recomputed),
              "plugin-reported score == monolithic full recompute");
        check(s.score.hard == 0, "plugin solve reaches 0hard");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) Raw costs by hand: one positive and one zero case per constraint,
// read through constraintCosts() (unweighted, before exponent and level).
// -------------------------------------------------------------------------
std::vector<double> rawCosts(TournamentSchedule &s, const std::string &name)
{
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(s);
    calculator.calculateScore();
    for (const auto &entry : calculator.constraintCosts())
    {
        if (entry.name == name)
            return entry.unitCosts;
    }
    return {};
}

void checkRaw(const std::string &label,
              TournamentSchedule &s,
              const std::string &name,
              const std::vector<double> &expected)
{
    const std::vector<double> actual = rawCosts(s, name);
    bool ok = actual.size() == expected.size();
    for (std::size_t i = 0; ok && i < actual.size(); ++i)
        ok = std::fabs(actual[i] - expected[i]) < 1e-12;
    std::string got;
    for (double value : actual)
        got += (got.empty() ? "" : ", ") + std::to_string(value);
    check(ok, label + " (raw {" + got + "})");
}

void verifyRawCostsByHand()
{
    std::printf("== (g) Raw costs by hand ==\n");
    {  // teamConflictCost, per team: T0 three times on D0 -> C(3, 2) = 3
        // pairs (its single D1 game adds none); T1 once on D1 -> 0.
        TournamentSchedule positive = makeBase(/*teams*/ 2, /*days*/ 2);
        addAssignment(positive, 0, 0);
        addAssignment(positive, 0, 0);
        addAssignment(positive, 0, 0);
        addAssignment(positive, 1, 0);
        addAssignment(positive, 1, 1);
        checkRaw("teamConflictCost: 3 same-day pairs on T0",
                 positive,
                 "teamConflictCost",
                 {3.0, 0.0});
        // Every team at most once per day.
        TournamentSchedule zero = makeBase(/*teams*/ 2, /*days*/ 2);
        addAssignment(zero, 0, 0);
        addAssignment(zero, 1, 0);
        addAssignment(zero, 0, 1);
        checkRaw("teamConflictCost: nobody plays twice a day",
                 zero,
                 "teamConflictCost",
                 {0.0, 0.0});
    }
    {  // teamUnavailabilityCost, per team: two facts on (T0, D1), where T0
        // plays twice -> 2 (per fact, not per assignment); T1's fact is on D0
        // but it plays D1 -> 0.
        TournamentSchedule positive = makeBase(/*teams*/ 2, /*days*/ 2);
        positive.unavailabilityPenalties.push_back({0, 1});
        positive.unavailabilityPenalties.push_back({0, 1});
        positive.unavailabilityPenalties.push_back({1, 0});
        addAssignment(positive, 1, 0);
        addAssignment(positive, 1, 0);
        addAssignment(positive, 1, 1);
        checkRaw("teamUnavailabilityCost: two facts hit on T0",
                 positive,
                 "teamUnavailabilityCost",
                 {2.0, 0.0});
        // The same facts, every team playing on an available day.
        TournamentSchedule zero = makeBase(/*teams*/ 2, /*days*/ 2);
        zero.unavailabilityPenalties.push_back({0, 1});
        zero.unavailabilityPenalties.push_back({1, 0});
        addAssignment(zero, 0, 0);
        addAssignment(zero, 1, 1);
        checkRaw("teamUnavailabilityCost: no fact hit",
                 zero,
                 "teamUnavailabilityCost",
                 {0.0, 0.0});
    }
    {  // assignmentCountImbalanceCost (global): loads {2, 1} ->
        // sqrt(5 - 9/2) = sqrt(1/2) = 0.707107 (6 s.f.).
        TournamentSchedule positive = makeBase(/*teams*/ 2, /*days*/ 3);
        addAssignment(positive, 0, 0);
        addAssignment(positive, 1, 0);
        addAssignment(positive, 2, 1);
        checkRaw("assignmentCountImbalanceCost: loads {2, 1}",
                 positive,
                 "assignmentCountImbalanceCost",
                 {0.707107});
        // Loads {1, 1} (an unloaded third team does not enter the balance).
        TournamentSchedule zero = makeBase(/*teams*/ 3, /*days*/ 2);
        addAssignment(zero, 0, 0);
        addAssignment(zero, 1, 1);
        checkRaw("assignmentCountImbalanceCost: loads {1, 1}",
                 zero,
                 "assignmentCountImbalanceCost",
                 {0.0});
    }
    {  // confrontationCountImbalanceCost (global): T0, T1, T2, T2 on D0 ->
        // pair counts (0,1)=1 (0,2)=2 (1,2)=2: sqrt(9 - 25/3) = sqrt(2/3)
        // = 0.816497.
        TournamentSchedule positive = makeBase(/*teams*/ 3, /*days*/ 1);
        addAssignment(positive, 0, 0);
        addAssignment(positive, 0, 1);
        addAssignment(positive, 0, 2);
        addAssignment(positive, 0, 2);
        checkRaw("confrontationCountImbalanceCost: pair counts {1, 2, 2}",
                 positive,
                 "confrontationCountImbalanceCost",
                 {0.816497});
        // Every pair meets once.
        TournamentSchedule zero = makeBase(/*teams*/ 3, /*days*/ 1);
        addAssignment(zero, 0, 0);
        addAssignment(zero, 0, 1);
        addAssignment(zero, 0, 2);
        checkRaw("confrontationCountImbalanceCost: pair counts {1, 1, 1}",
                 zero,
                 "confrontationCountImbalanceCost",
                 {0.0});
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) The catalog read off the default assembly == the components' constants.
// -------------------------------------------------------------------------
template <typename Component>
bool rowIs(const optimaru::ConstraintInfo &row,
           const char *name,
           optimaru::Level level,
           double weight,
           bool isGlobal)
{
    return row.name == name && row.defaultLevel == Component::kDefaultLevel
           && row.defaultWeight == Component::kDefaultWeight
           && row.defaultLevel == level && row.defaultWeight == weight
           && row.levelFlippable && !row.reward && row.isGlobal == isGlobal
           && row.defaultEnabled;
}

void verifyCatalog()
{
    std::printf("== (h) Catalog == component constants ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 4, "4 catalog rows");
    if (rows.size() != 4)
        return;
    // Unit constraints first, then the globals, in emplace order: the
    // list_constraints() order.
    check(rowIs<TeamConflictCost>(
              rows[0], "teamConflictCost", Level::Hard, 1.0, false)
              && rowIs<TeamUnavailabilityCost>(
                  rows[1], "teamUnavailabilityCost", Level::Hard, 1.0, false)
              && rowIs<AssignmentCountImbalanceCost>(
                  rows[2],
                  "assignmentCountImbalanceCost",
                  Level::Medium,
                  1.0,
                  true)
              && rowIs<ConfrontationCountImbalanceCost>(
                  rows[3],
                  "confrontationCountImbalanceCost",
                  Level::Soft,
                  1.0,
                  true),
          "info() == the components' kDefaultWeight / kDefaultLevel");
    check(kMediumFold.kind == optimaru::MediumFold::Kind::IntoSoft
              && kMediumFold.unit == kMediumScale && kMediumScale == 1e6,
          "the domain folds medium into soft at 1e6");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The medium level: assignmentCountImbalanceCost.
// -------------------------------------------------------------------------
DecomposedCalculator::LevelScores levelsWith(ConstraintAssembly assembly,
                                             TournamentSchedule &s)
{
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    calculator.resetWorkingSolution(s);
    calculator.calculateScore();
    return calculator.levelScores();
}

bool near(double actual, double expected)
{
    return std::fabs(actual - expected) < 1e-9;
}

void verifyMediumLevel()
{
    std::printf("== (i) Medium level ==\n");
    // D0: T0, T1, T2; D1: T1, T2; D2: T1; D3: T1. Loads {1, 4, 2}:
    // sqrt(21 - 49/3) = sqrt(14/3) = 2.16025 medium. Confrontations
    // (0,1)=1 (0,2)=1 (1,2)=2: sqrt(6 - 16/3) = sqrt(2/3) = 0.816497 soft.
    // Nobody plays twice a day: 0 hard.
    TournamentSchedule s = makeBase(/*teams*/ 3, /*days*/ 4);
    addAssignment(s, 0, 0);
    addAssignment(s, 0, 1);
    addAssignment(s, 0, 2);
    addAssignment(s, 1, 1);
    addAssignment(s, 1, 2);
    addAssignment(s, 2, 1);
    addAssignment(s, 3, 1);
    const double medium = 2.16025;
    const double soft = 0.816497;

    const Score monolithic = ScoreCalculator::calculateScore(s);
    check(monolithic.hard == 0 && near(monolithic.soft, -(soft + medium * 1e6)),
          "monolithic: medium folded into soft x 1e6");
    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        const Score engine = scoreWith(calculator, s);
        check(engine.hard == monolithic.hard && engine.soft == monolithic.soft,
              "default assembly: the fold equals the monolithic score "
              "bit-for-bit");
        const auto levels = calculator.levelScores();
        check(levels.hard == 0 && near(levels.medium, -medium)
                  && near(levels.soft, -soft),
              "levelScores() reports the unfolded levels");
    }
    const char *imbalance = "assignmentCountImbalanceCost";
    {
        // A global hard contribution is rounded once: llround(2.16025) = 2.
        const auto levels = levelsWith(defaultConstraints().hard(imbalance), s);
        check(levels.hard == -2 && levels.medium == 0.0
                  && near(levels.soft, -soft),
              "hard(): one hard unit per raw unit (rounded)");
    }
    {
        const auto levels = levelsWith(defaultConstraints().soft(imbalance), s);
        check(levels.hard == 0 && levels.medium == 0.0
                  && near(levels.soft, -(soft + medium)),
              "soft(): one soft unit per raw unit, outside the fold");
    }
    {
        const auto levels
            = levelsWith(defaultConstraints().weight(imbalance, 3), s);
        check(levels.hard == 0 && near(levels.medium, -3.0 * medium)
                  && near(levels.soft, -soft),
              "weight 3 stays in medium units");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().weight(imbalance, 3).into(calculator);
        const Score engine = scoreWith(calculator, s);
        check(engine.hard == 0
                  && near(engine.soft, -(soft + 3.0 * medium * 1e6)),
              "weight 3 folds as 3 medium units x 1e6");
    }
    std::printf("\n");
}

// Incremental == full recompute with the medium term overridden, through the
// ScoreDirector protocol.
void verifyMediumIncremental()
{
    std::printf("== (i) Medium overrides: incremental == full ==\n");
    const char *imbalance = "assignmentCountImbalanceCost";
    const std::vector<
        std::pair<std::string, std::function<ConstraintAssembly()>>>
        variants{
            {"default", [] { return defaultConstraints(); }},
            {"hard", [=] { return defaultConstraints().hard(imbalance); }},
            {"soft", [=] { return defaultConstraints().soft(imbalance); }},
            {"weight 3",
             [=] { return defaultConstraints().weight(imbalance, 3); }},
        };
    for (const auto &variant : variants)
    {
        TournamentSchedule s = buildRandomInstance(/*teams*/ 5,
                                                   /*days*/ 6,
                                                   /*perDay*/ 3,
                                                   /*seed*/ 17);
        DecomposedCalculator calculator;
        variant.second().into(calculator);
        optimaru::ScoreDirector<TournamentSchedule> director(s, calculator);
        std::mt19937_64 rng(41);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const int assignmentCount = static_cast<int>(s.teamAssignments.size());
        int mismatches = 0;
        int mediumSteps = 0;
        for (int step = 0; step < 600; ++step)
        {
            const int assignment = static_cast<int>(rng() % assignmentCount);
            const int value = unit(rng) < 0.10
                                  ? -1
                                  : static_cast<int>(rng() % s.teamCount());
            director.changeVariable(assignment, value);
            const Score incremental = director.calculateScore();
            DecomposedCalculator fresh;
            variant.second().into(fresh);
            const Score full = scoreWith(fresh, s);
            if (fresh.levelScores().medium != 0.0)
                ++mediumSteps;
            if (incremental.hard != full.hard || incremental.soft != full.soft)
                ++mismatches;
        }
        check(mismatches == 0,
              "medium " + variant.first
                  + ": 600 random moves, incremental == full recompute");
        if (variant.first == "default" || variant.first == "weight 3")
            check(mediumSteps > 0,
                  variant.first + ": the walk visits medium states");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with addInstances(), as the Python bindings
// build it from a pick list.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string &name)
{
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// The (i) schedule plus three unavailability facts: T0 twice on D0 (it
// plays D0) and T2 on D3 (it does not). Raw per team (T0, T1, T2):
// teamUnavailabilityCost [2, 0, 0] evaluated on T0 and T2 only,
// teamConflictCost [0, 0, 0] on every team; the globals 2.16025 (medium) and
// 0.816497 (soft).
TournamentSchedule repeatInstance()
{
    TournamentSchedule s = makeBase(/*teams*/ 3, /*days*/ 4);
    addAssignment(s, 0, 0);
    addAssignment(s, 0, 1);
    addAssignment(s, 0, 2);
    addAssignment(s, 1, 1);
    addAssignment(s, 1, 2);
    addAssignment(s, 2, 1);
    addAssignment(s, 3, 1);
    s.unavailabilityPenalties.push_back({0, 0});
    s.unavailabilityPenalties.push_back({0, 0});
    s.unavailabilityPenalties.push_back({2, 3});
    return s;
}

ConstraintAssembly repeatedAssembly()
{
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("teamConflictCost", 1.0);
    assembly.addInstances(std::move(copyOf("teamConflictCost")
                                        .weight("teamConflictCost", 2.0)
                                        .offset("teamConflictCost", 1.0)
                                        .clip("teamConflictCost", false)));
    assembly.addInstances(std::move(copyOf("teamUnavailabilityCost")
                                        .exponent("teamUnavailabilityCost", 2.0)
                                        .offset("teamUnavailabilityCost", -1.0)
                                        .clip("teamUnavailabilityCost", false)));
    assembly.addInstances(
        std::move(copyOf("assignmentCountImbalanceCost")
                      .exponent("assignmentCountImbalanceCost", 2.0)
                      .offset("assignmentCountImbalanceCost", 1.0)));
    assembly.addInstances(
        std::move(copyOf("assignmentCountImbalanceCost")
                      .soft("assignmentCountImbalanceCost")
                      .weight("assignmentCountImbalanceCost", 3.0)
                      .offset("assignmentCountImbalanceCost", -0.5)
                      .clip("assignmentCountImbalanceCost", false)));
    assembly.addInstances(
        std::move(copyOf("confrontationCountImbalanceCost")
                      .weight("confrontationCountImbalanceCost", 0.25)
                      .offset("confrontationCountImbalanceCost", 2.0)
                      .clip("confrontationCountImbalanceCost", false)));
    return assembly;
}

void verifyRepeatsAndOffsets()
{
    std::printf("== (j) Repeated components, offset / clip ==\n");
    const double medium = 2.16025;
    const double soft = 0.816497;
    {
        TournamentSchedule s = repeatInstance();
        ConstraintAssembly twice = copyOf("teamUnavailabilityCost");
        twice.addInstances(std::move(copyOf("teamUnavailabilityCost")
                                         .weight("teamUnavailabilityCost", 2.0)
                                         .exponent("teamUnavailabilityCost", 2.0)));
        DecomposedCalculator calculator;
        std::move(twice).into(calculator);
        calculator.resetWorkingSolution(s);
        calculator.calculateScore();
        const auto levels = calculator.levelScores();
        check(levels.hard == -10 && levels.medium == 0.0 && levels.soft == 0.0,
              "facts hit + 2 x their square per team: -10 hard");
        const auto entries = calculator.constraintCosts();
        check(entries.size() == 2 && entries[0].unitCosts == entries[1].unitCosts
                  && entries[0].unitCosts == std::vector<double>{2, 0, 0}
                  && entries[0].scoped && entries[1].scoped
                  && entries[0].units == std::vector<int>{0, 2}
                  && entries[1].weight == 2.0 && entries[1].exponent == 2.0
                  && entries[1].defaultWeight == 1.0,
              "two scoped instances over T0 and T2, each with its own knobs");
    }
    {
        TournamentSchedule s = repeatInstance();
        check(levelsWith(copyOf("teamUnavailabilityCost")
                             .offset("teamUnavailabilityCost", 3.0)
                             .clip("teamUnavailabilityCost", false),
                         s)
                      .hard
                  == 4,
              "unavailability literal offset 3: (2 - 3) + (0 - 3) on T0, T2 only");
        check(levelsWith(copyOf("teamUnavailabilityCost")
                             .offset("teamUnavailabilityCost", -1.0),
                         s)
                      .hard
                  == -3,
              "unavailability threshold offset -1: T0 only, 2 + 1");
        check(levelsWith(copyOf("teamConflictCost")
                             .weight("teamConflictCost", 3.0)
                             .offset("teamConflictCost", 0.5)
                             .clip("teamConflictCost", false),
                         s)
                      .hard
                  == 6,
              "conflict literal offset 0.5 at weight 3: llround(-1.5) x 3 teams");
        const auto confrontation
            = levelsWith(copyOf("confrontationCountImbalanceCost")
                             .offset("confrontationCountImbalanceCost", 1.0)
                             .clip("confrontationCountImbalanceCost", false),
                         s);
        check(confrontation.hard == 0 && near(confrontation.soft, -(soft - 1.0)),
              "confrontation literal offset 1 on the single total");
        const auto assignment
            = levelsWith(copyOf("assignmentCountImbalanceCost")
                             .offset("assignmentCountImbalanceCost", 2.0),
                         s);
        check(near(assignment.medium, -(medium - 2.0)),
              "assignment-imbalance threshold offset 2: medium -(2.16025 - 2)");
        const auto promoted
            = levelsWith(copyOf("assignmentCountImbalanceCost")
                             .hard("assignmentCountImbalanceCost")
                             .offset("assignmentCountImbalanceCost", 3.0)
                             .clip("assignmentCountImbalanceCost", false),
                         s);
        check(promoted.hard == 1 && promoted.medium == 0.0,
              "a hard literal offset 3 on the total: llround(-0.83975) -> +1 hard");
    }
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        TournamentSchedule s = buildRandomInstance(/*teams*/ 5,
                                                   /*days*/ 6,
                                                   /*perDay*/ 3,
                                                   /*seed*/ 7);
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::ScoreDirector<TournamentSchedule> director(s, calculator);
        std::mt19937_64 rng(321);
        const int assignmentCount = static_cast<int>(s.teamAssignments.size());
        const int valueCount = s.teamCount();
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        int mismatches = 0;
        int mediumSteps = 0;
        double drift = 0.0;
        const int steps = 3000;
        for (int step = 0; step < steps; ++step)
        {
            if (unit(rng) < 0.75)
            {
                const int assignment = static_cast<int>(rng() % assignmentCount);
                const int value = unit(rng) < 0.10
                                      ? -1
                                      : static_cast<int>(rng() % valueCount);
                director.changeVariable(assignment, value);
            }
            else
            {
                const int a = static_cast<int>(rng() % assignmentCount);
                const int b = static_cast<int>(rng() % assignmentCount);
                const int va = s.teamAssignments[a].team;
                const int vb = s.teamAssignments[b].team;
                director.changeVariable(a, vb);
                director.changeVariable(b, va);
            }
            const Score incremental = director.calculateScore();
            TournamentSchedule shadow = s;
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            const Score full = scoreWith(fresh, shadow);
            if (fresh.levelScores().medium != 0.0)
                ++mediumSteps;
            drift = std::max(drift, std::fabs(full.soft - incremental.soft));
            if (!optimaru::scoresAgree(incremental, full))
                ++mismatches;
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0,
              std::to_string(steps)
                  + " random moves with repeated, shifted components: "
                    "incremental == fresh");
        check(mediumSteps > 0, "the walk visits medium states");
    }
    {
        TournamentSchedule s = buildRandomInstance(/*teams*/ 5,
                                                   /*days*/ 6,
                                                   /*perDay*/ 3,
                                                   /*seed*/ 11);
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(2);
        config.randomSeed = 5;
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        bool corrupted = false;
        try
        {
            Solver solver(config);
            solver.solve(s, calculator);
        }
        catch (const optimaru::ScoreCorruptionError &e)
        {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted, "2s FULL_ASSERT solve with repeated, shifted components");
        DecomposedCalculator fresh;
        repeatedAssembly().into(fresh);
        check(!corrupted && sameScore(s.score, scoreWith(fresh, s)),
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
    verifyRawCostsByHand();
    verifyCatalog();
    verifyMediumLevel();
    verifyMediumIncremental();
    verifyRepeatsAndOffsets();

    if (failures == 0)
    {
        std::printf("All tournament-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d tournament-scheduling plugin check(s) FAILED.\n", failures);
    return 1;
}

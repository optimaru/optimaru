// Plugin-assembly (ts/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (unassigned included)
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: each component in isolation, hand-computed
//   (d) weight / isHard overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(schedule, calculator) under FULL_ASSERT
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
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

    {  // oneAssignmentPerDatePerTeam alone: three assignments of one team on
        // one day -> C(3, 2) = 3 pairs.
        TournamentSchedule s = makeBase(/*teams*/ 2, /*days*/ 2);
        addAssignment(s, 0, 0);
        addAssignment(s, 0, 0);
        addAssignment(s, 0, 0);
        DecomposedCalculator calculator;
        calculator.emplace<OneAssignmentPerDatePerTeam>();
        checkScore("oneAssignmentPerDatePerTeam alone",
                   scoreWith(calculator, s),
                   -3,
                   0.0);
    }
    {  // unavailabilityPenalty alone: one fact, one hit — and a second
        // assignment in the same cell still costs one (per fact, not per
        // assignment).
        TournamentSchedule s = makeBase(/*teams*/ 2, /*days*/ 2);
        s.unavailabilityPenalties.push_back({0, 1});
        addAssignment(s, 1, 0);
        addAssignment(s, 1, 0);
        DecomposedCalculator calculator;
        calculator.emplace<UnavailabilityPenaltyConstraint>();
        checkScore("unavailabilityPenalty alone (per fact)",
                   scoreWith(calculator, s),
                   -1,
                   0.0);
    }
    {  // The same fact with the team playing elsewhere: silent.
        TournamentSchedule s = makeBase(/*teams*/ 2, /*days*/ 2);
        s.unavailabilityPenalties.push_back({0, 1});
        addAssignment(s, 0, 0);
        DecomposedCalculator calculator;
        calculator.emplace<UnavailabilityPenaltyConstraint>();
        checkScore("unavailabilityPenalty silent off the day",
                   scoreWith(calculator, s),
                   0,
                   0.0);
    }
    {  // fairAssignmentCountPerTeam alone: loads 2 and 0 -> only the loaded
        // team enters the balance, so the unfairness is 0.
        TournamentSchedule s = makeBase(/*teams*/ 2, /*days*/ 2);
        addAssignment(s, 0, 0);
        addAssignment(s, 1, 0);
        DecomposedCalculator calculator;
        calculator.emplace<FairAssignmentCountPerTeam>();
        checkScore("fairAssignmentCountPerTeam, single loaded team",
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
        calculator.emplace<FairAssignmentCountPerTeam>();
        const double expected
            = -ScoreCalculator::unfairnessFromAggregates(3, 5, 2)
              * kMediumScale;
        checkScore("fairAssignmentCountPerTeam, loads 2 and 1",
                   scoreWith(calculator, s),
                   0,
                   expected);
    }
    {  // evenlyConfrontationCount alone: one confronting pair -> a single
        // loaded item, so the unfairness is 0 (and the medium level is not
        // in this assembly at all).
        TournamentSchedule s = makeBase(/*teams*/ 2, /*days*/ 1);
        addAssignment(s, 0, 0);
        addAssignment(s, 0, 1);
        DecomposedCalculator calculator;
        calculator.emplace<EvenlyConfrontationCount>();
        checkScore("evenlyConfrontationCount, single pair",
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
        calculator.emplace<OneAssignmentPerDatePerTeam>(10.0);
        checkScore("oneAssignmentPerDatePerTeam re-weighted to 10",
                   scoreWith(calculator, s),
                   -30,
                   0.0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<OneAssignmentPerDatePerTeam>();
        component->isHard = false;
        calculator.add(std::move(component));
        checkScore("oneAssignmentPerDatePerTeam demoted to soft",
                   scoreWith(calculator, s),
                   0,
                   -3.0);
    }
    {
        // exponent 2 shapes the raw pair count (3) before the weight.
        DecomposedCalculator calculator;
        auto component = std::make_unique<OneAssignmentPerDatePerTeam>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("oneAssignmentPerDatePerTeam with exponent 2",
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

    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
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

}  // namespace

int main()
{
    verifyAssemblyParity();
    verifyIncremental();
    verifyPartialAssemblies();
    verifyOverrides();
    verifyConstraintTotals();
    verifyFullAssertSolve();

    if (failures == 0)
    {
        std::printf("All tournament-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d tournament-scheduling plugin check(s) FAILED.\n", failures);
    return 1;
}

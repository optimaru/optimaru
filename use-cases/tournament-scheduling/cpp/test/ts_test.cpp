// Tournament-scheduling unit tests (engine test/ style: check() + non-zero exit
// on any failure):
//   (a) every constraint verified against hand-computed scores (the Java
//       TournamentScheduleConstraintProviderTest scenarios and more)
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's ScoreDirector protocol (before/set/after)
//   (c) a small instance solved under EnvironmentMode::FULL_ASSERT
//   (d) pinned assignments keep their team through a solve
//   (e) the loaded-team-count macro move reaches an optimum that no
//       change/swap move can reach
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "optimaru/api/score/director/score_director.hpp"
#include "ts/domain.hpp"
#include "ts/incremental_calculator.hpp"
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
    const bool ok = (actual.hard == expectedHard
                     && std::fabs(actual.soft - expectedSoft) < 1e-6);
    std::printf("  [%s] %-44s got=%s expected=%ldhard/%gsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedSoft);
    if (!ok)
        ++failures;
}

// Base problem: `teamCount` anonymous teams and `dayCount` days, no
// assignments yet.
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

// -------------------------------------------------------------------------
// (a) Each constraint against hand-computed scores. Reference values:
//   sqrt(1/2)  -> 0.707107   sqrt(2)    -> 1.41421
//   sqrt(2/3)  -> 0.816497   sqrt(10/7) -> 1.19523   (6 s.f., HALF_EVEN)
// The engine soft = -(medium * 1e6 + soft), see ts::kMediumScale.
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Verifying constraints against hand-computed scores "
                "==\n");

    // LoadBalance::unfairness building block itself.
    check(ScoreCalculator::unfairness({}) == 0.0, "unfairness({}) == 0");
    check(ScoreCalculator::unfairness({5}) == 0.0, "unfairness({5}) == 0");
    check(std::fabs(ScoreCalculator::unfairness({3, 2}) - 0.707107) < 1e-12,
          "unfairness({3,2}) == 0.707107");
    check(std::fabs(ScoreCalculator::unfairness({1, 1, 2}) - 0.816497) < 1e-12,
          "unfairness({1,1,2}) == 0.816497");
    check(std::fabs(ScoreCalculator::unfairness({11, 11, 10, 10, 10, 10, 10})
                    - 1.19523)
              < 1e-12,
          "unfairness({11,11,10,10,10,10,10}) == 1.19523");

    // hard 1: oneAssignmentPerDatePerTeam — the Java test scenario: T0 three
    // times on D0 (3 pairs), T1 on D1 and D2 (0 pairs). Medium: loads {3, 2}
    // -> 0.707107; no confrontations (same-team pairs are excluded).
    {
        TournamentSchedule s = makeBase(3, 3);
        addAssignment(s, 0, 0);
        addAssignment(s, 0, 0);
        addAssignment(s, 0, 0);
        addAssignment(s, 1, 1);
        addAssignment(s, 2, 1);
        checkScore("oneAssignmentPerDatePerTeam (Java test)",
                   ScoreCalculator::calculateScore(s),
                   -3,
                   -707107.0);
    }
    // hard 2: unavailabilityPenalty — the Java test scenario: penalties
    // (T0, D0) and (T1, D1) both hit (+ T1 twice on D1 = 1 hard pair).
    // Medium: loads {1, 3} -> sqrt(2) = 1.41421.
    {
        TournamentSchedule s = makeBase(3, 3);
        addAssignment(s, 0, 0);
        addAssignment(s, 1, 1);
        addAssignment(s, 1, 1);
        addAssignment(s, 2, 1);
        s.unavailabilityPenalties.push_back({0, 0});
        s.unavailabilityPenalties.push_back({1, 1});
        checkScore("unavailabilityPenalty (Java test)",
                   ScoreCalculator::calculateScore(s),
                   -3,
                   -1414210.0);
    }
    // hard 2: a penalty whose (team, day) has no assignment costs nothing.
    {
        TournamentSchedule s = makeBase(3, 3);
        addAssignment(s, 0, 0);
        s.unavailabilityPenalties.push_back({2, 0});
        checkScore("unavailabilityPenalty miss",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0.0);
    }
    // hard 2: duplicate penalty facts each count (Java forEach + ifExists).
    {
        TournamentSchedule s = makeBase(3, 3);
        addAssignment(s, 0, 0);
        s.unavailabilityPenalties.push_back({0, 0});
        s.unavailabilityPenalties.push_back({0, 0});
        checkScore("unavailabilityPenalty duplicate facts",
                   ScoreCalculator::calculateScore(s),
                   -2,
                   0.0);
    }
    // medium 3: fairAssignmentCountPerTeam — the Java test scenario: one
    // assignment each -> perfectly fair, everything 0.
    {
        TournamentSchedule s = makeBase(3, 3);
        addAssignment(s, 0, 0);
        addAssignment(s, 1, 1);
        addAssignment(s, 2, 2);
        checkScore("fairAssignmentCountPerTeam fair",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0.0);
        // T2 twice while everyone else once: loads {1, 1, 2} -> 0.816497.
        // Confrontations: only (T0, T2) on D0, a single loaded pair -> 0.
        addAssignment(s, 0, 2);
        checkScore("fairAssignmentCountPerTeam unfair",
                   ScoreCalculator::calculateScore(s),
                   0,
                   -816497.0);
    }
    // soft 4: evenlyConfrontationCount — the Java test scenario: three teams
    // meet pairwise once on D0 -> perfectly even, everything 0.
    {
        TournamentSchedule s = makeBase(3, 3);
        addAssignment(s, 0, 0);
        addAssignment(s, 0, 1);
        addAssignment(s, 0, 2);
        checkScore("evenlyConfrontationCount even",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0.0);
        // + T2 again on D0: confrontations (0,1)=1 (0,2)=2 (1,2)=2 ->
        // 0.816497 soft; loads {1,1,2} -> 0.816497 medium; T2's day pair -> 1
        // hard.
        addAssignment(s, 0, 2);
        checkScore("evenlyConfrontationCount uneven",
                   ScoreCalculator::calculateScore(s),
                   -1,
                   -816497.816497);
    }
    // soft 4: confrontation counts multiply per assignment pair: T0 twice +
    // T1 once on D0 -> the (T0, T1) pair has 2 confrontations (single loaded
    // pair -> unfairness 0); T0's own day pair -> 1 hard; loads {2, 1} ->
    // 0.707107.
    {
        TournamentSchedule s = makeBase(3, 3);
        addAssignment(s, 0, 0);
        addAssignment(s, 0, 0);
        addAssignment(s, 0, 1);
        checkScore("confrontation multiplicity",
                   ScoreCalculator::calculateScore(s),
                   -1,
                   -707107.0);
    }
    // Unassigned assignments contribute nothing (also not to penalties).
    {
        TournamentSchedule s = makeBase(3, 3);
        s.unavailabilityPenalties.push_back({0, 0});
        addAssignment(s, 0, -1);
        addAssignment(s, 0, 1);
        checkScore("unassigned contributes nothing",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0.0);
    }
    // Pinned assignments are scored like any other assignment.
    {
        TournamentSchedule s = makeBase(3, 3);
        addAssignment(s, 0, 0);
        s.teamAssignments.back().pinned = true;
        addAssignment(s, 0, 0);
        checkScore("pinned assignment still scored",
                   ScoreCalculator::calculateScore(s),
                   -1,
                   0.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random mid-size instance for the incremental cross-check and the solves.
// -------------------------------------------------------------------------
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

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves (including unassignments), driven through the ScoreDirector so
// the before/set/after hook contract is exercised exactly like in the
// engine.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Verifying incremental scoring == full recompute ==\n");
    TournamentSchedule s = buildRandomInstance(/*teams*/ 5,
                                               /*days*/ 6,
                                               /*perDay*/ 3,
                                               /*seed*/ 7);

    TournamentSchedulingIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<TournamentSchedule> director(s, calc);
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
            // ChangeMove; 10% of the time to unassigned (-1), like the
            // construction heuristic's starting state.
            const int assignment = static_cast<int>(rng() % assignmentCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(assignment, value);
        }
        else
        {
            // SwapMove.
            const int a = static_cast<int>(rng() % assignmentCount);
            const int b = static_cast<int>(rng() % assignmentCount);
            const int va = s.teamAssignments[a].team;
            const int vb = s.teamAssignments[b].team;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < assignmentCount; ++i)
        {
            shadow.teamAssignments[i].team = s.teamAssignments[i].team;
        }
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = director.calculateScore();
        if (full.hard != incremental.hard
            || std::fabs(full.soft - incremental.soft) > 1e-9)
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
// (c) FULL_ASSERT solve: the engine cross-checks the incremental score
// against a full reset + recompute after every single move and throws
// ScoreCorruptionError on any divergence.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (c) FULL_ASSERT solve ==\n");
    TournamentSchedule s = buildRandomInstance(/*teams*/ 5,
                                               /*days*/ 5,
                                               /*perDay*/ 3,
                                               /*seed*/ 11);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    // Kick early so the LNS ruin/recreate path also runs under FULL_ASSERT.
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    bool corrupted = false;
    Score final{};
    try
    {
        Solver solver(config);
        solver.solve(s);
        final = s.score;
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(s);
        check(final.hard == recomputed.hard
                  && std::fabs(final.soft - recomputed.soft) < 1e-9,
              "solver-reported score == full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Pinned assignments keep their team through a solve, every assignment
// ends up assigned, and the small instance reaches 0hard.
// -------------------------------------------------------------------------
void verifyPinnedSolve()
{
    std::printf("== (d) Pinned assignment survives a solve ==\n");
    TournamentSchedule s = buildRandomInstance(/*teams*/ 6,
                                               /*days*/ 6,
                                               /*perDay*/ 3,
                                               /*seed*/ 21);
    s.teamAssignments[0].team = 3;
    s.teamAssignments[0].pinned = true;

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(1);
    config.randomSeed = 5;
    Solver solver(config);
    solver.solve(s);

    check(s.teamAssignments[0].team == 3, "pinned assignment kept its team");
    bool allAssigned = true;
    for (const TeamAssignment &assignment : s.teamAssignments)
    {
        if (assignment.team < 0)
            allAssigned = false;
    }
    check(allAssigned, "every assignment assigned after the solve");
    check(s.score.hard == 0, "small instance reaches 0hard");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) The loaded-team-count macro move: fairAssignmentCountPerTeam balances
// over the LOADED teams only, so 72 assignments over 7 teams can never beat
// unfairness sqrt(10/7), while the same 72 over 4 teams (18 each, every pair
// meeting 18 times) scores a perfect 0hard/0soft. Reaching it means emptying
// three whole teams, which no change/swap move can do — every intermediate
// state is far worse than both ends. Without Config::teamCountMoves the
// solver stalls at -1195232.563480soft here.
// -------------------------------------------------------------------------
void verifyTeamCountMove()
{
    std::printf("== (e) Loaded-team-count macro move ==\n");
    TournamentSchedule s = makeBase(7, 18);
    for (int d = 0; d < 18; ++d)
    {
        for (int i = 0; i < 4; ++i)
            addAssignment(s, d);
    }

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 42;
    Solver solver(config);
    solver.solve(s);

    std::vector<std::int64_t> loads(s.teamCount(), 0);
    for (const TeamAssignment &assignment : s.teamAssignments)
    {
        if (assignment.team >= 0)
            ++loads[assignment.team];
    }
    int loadedTeams = 0;
    for (std::int64_t load : loads)
        loadedTeams += load > 0 ? 1 : 0;

    check(s.score.hard == 0, "penalty-free instance stays feasible");
    check(loadedTeams == 4,
          "search empties the teams that unbalance the load ("
              + std::to_string(loadedTeams) + " teams loaded)");
    check(s.score.soft == 0.0,
          "reaches the 0hard/0soft optimum (got " + std::to_string(s.score.soft)
              + ")");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifyPinnedSolve();
    verifyTeamCountMove();

    if (failures == 0)
    {
        std::printf("All tournament-scheduling checks PASSED.\n");
        return 0;
    }
    std::printf("%d tournament-scheduling check(s) FAILED.\n", failures);
    return 1;
}

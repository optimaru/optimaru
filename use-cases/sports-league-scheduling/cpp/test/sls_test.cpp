// Sports-league-scheduling unit tests (engine test/ style: check() + non-zero
// exit on any failure):
//   (a) every constraint verified against hand-computed scores (the Java
//       SportsLeagueSchedulingConstraintProviderTest cases, adapted to the
//       full-score world: each case isolates its constraint by construction)
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's ScoreDirector protocol (before/set/after)
//   (c) a small instance solved under EnvironmentMode::FULL_ASSERT
//   (d) a double round robin reaches 0hard within a short solve
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "sls/domain.hpp"
#include "sls/incremental_calculator.hpp"
#include "sls/score_calculator.hpp"
#include "sls/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace sls;

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
                     && std::fabs(actual.soft - expectedSoft) < 1e-9);
    std::printf("  [%s] %-42s got=%s expected=%ldhard/%gsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedSoft);
    if (!ok)
        ++failures;
}

// Base problem: `teamCount` teams and rounds with the given indices (all
// non-weekend unless flagged later). Distances default to zero (prepare
// fills the matrix); hop tests install dist(a, b) = 10 * (a+1) + (b+1).
LeagueSchedule makeBase(int teamCount, const std::vector<int> &roundIndices)
{
    LeagueSchedule s;
    for (int t = 0; t < teamCount; ++t)
        s.teams.push_back(
            {"T" + std::to_string(t), "Team " + std::to_string(t)});
    for (int index : roundIndices)
    {
        Round round;
        round.index = index;
        s.rounds.push_back(round);
    }
    return s;
}

LeagueSchedule makeBaseContiguous(int teamCount, int roundCount)
{
    std::vector<int> indices(roundCount);
    for (int i = 0; i < roundCount; ++i)
        indices[i] = i;
    return makeBase(teamCount, indices);
}

void useSyntheticDistances(LeagueSchedule &s)
{
    const int n = s.teamCount();
    s.distanceToTeam.assign(n, std::vector<int>(n, 0));
    for (int a = 0; a < n; ++a)
    {
        for (int b = 0; b < n; ++b)
        {
            if (a != b)
                s.distanceToTeam[a][b] = 10 * (a + 1) + (b + 1);
        }
    }
}

void addMatch(LeagueSchedule &s,
              int homeTeam,
              int awayTeam,
              int round = -1,
              bool classic = false)
{
    Match match;
    match.id = "M" + std::to_string(s.matches.size());
    match.homeTeam = homeTeam;
    match.awayTeam = awayTeam;
    match.round = round;
    match.classicMatch = classic;
    s.matches.push_back(match);
}

// -------------------------------------------------------------------------
// (a) Each constraint against hand-computed full scores. Every case is
// constructed so the other nine constraints contribute zero (zero distances
// unless the case is about hops; opponents chosen to avoid accidental runs).
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Verifying each constraint ==\n");

    // hard 1: two matches of one team on the same round index.
    {
        LeagueSchedule s = makeBaseContiguous(4, 2);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 1);  // unassigned: contributes nothing
        checkScore("sameDay both teams shared",
                   ScoreCalculator::calculateScore(s),
                   -1,
                   0);
    }
    {
        LeagueSchedule s = makeBaseContiguous(4, 2);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 2, 0);
        checkScore("sameDay one team shared",
                   ScoreCalculator::calculateScore(s),
                   -1,
                   0);
    }
    {
        LeagueSchedule s = makeBaseContiguous(4, 2);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 3, 0);
        checkScore(
            "sameDay disjoint teams", ScoreCalculator::calculateScore(s), 0, 0);
    }
    // hard 1: three matches of one team in a round -> C(3,2) pairs.
    {
        LeagueSchedule s = makeBaseContiguous(4, 2);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 2, 0);
        addMatch(s, 0, 3, 0);
        checkScore("sameDay triple", ScoreCalculator::calculateScore(s), -3, 0);
    }

    // hard 2: four consecutive home matches (opponents rotated so no away
    // run forms) -> penalized by the run length 4.
    {
        LeagueSchedule s = makeBaseContiguous(4, 5);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 2, 1);
        addMatch(s, 0, 3, 2);
        addMatch(s, 0, 1, 3);
        addMatch(s, 2, 0);  // unassigned
        checkScore("4 consecutive home matches",
                   ScoreCalculator::calculateScore(s),
                   -4,
                   0);
    }
    {
        LeagueSchedule s = makeBaseContiguous(4, 5);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 2, 1);
        addMatch(s, 0, 3, 2);
        checkScore("3 consecutive home matches ok",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0);
    }
    {
        LeagueSchedule s = makeBaseContiguous(4, 6);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 2, 1);
        addMatch(s, 0, 3, 3);
        addMatch(s, 0, 1, 4);
        checkScore("home matches split by a gap ok",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0);
    }
    {
        LeagueSchedule s = makeBaseContiguous(4, 6);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 2, 1);
        addMatch(s, 0, 3, 2);
        addMatch(s, 0, 1, 3);
        addMatch(s, 0, 2, 4);
        checkScore("5 consecutive home matches cost 5",
                   ScoreCalculator::calculateScore(s),
                   -5,
                   0);
    }
    // hard 2 + hard 1: two home matches on one round index count once in the
    // run (the reference model's consecutive-sequence collector dedupes the Round values), so
    // indices {0, 0, 1, 2} form a length-3 run — no consecutive penalty; only
    // the same-round pair costs one.
    {
        LeagueSchedule s = makeBaseContiguous(4, 4);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 2, 0);
        addMatch(s, 0, 3, 1);
        addMatch(s, 0, 1, 2);
        checkScore("duplicate index collapses in home run",
                   ScoreCalculator::calculateScore(s),
                   -1,
                   0);
    }
    // ... but with a fourth distinct index {0, 0, 1, 2, 3} the run reaches
    // length 4: penalty 4 plus the same-round pair.
    {
        LeagueSchedule s = makeBaseContiguous(4, 5);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 2, 0);
        addMatch(s, 0, 3, 1);
        addMatch(s, 0, 1, 2);
        addMatch(s, 0, 2, 3);
        checkScore("duplicate index run reaching 4",
                   ScoreCalculator::calculateScore(s),
                   -5,
                   0);
    }

    // hard 3: four consecutive away matches (hosts rotated).
    {
        LeagueSchedule s = makeBaseContiguous(4, 5);
        addMatch(s, 1, 0, 0);
        addMatch(s, 2, 0, 1);
        addMatch(s, 3, 0, 2);
        addMatch(s, 1, 0, 3);
        checkScore("4 consecutive away matches",
                   ScoreCalculator::calculateScore(s),
                   -4,
                   0);
    }

    // hard 4: the Java repeatMatchOnTheNextDay case — only the round-0 match
    // is followed by its reversed fixture on the next round index.
    {
        LeagueSchedule s = makeBaseContiguous(2, 7);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 0, 1);
        addMatch(s, 0, 1, 4);
        addMatch(s, 1, 0, 6);
        checkScore("repeat match on the next day",
                   ScoreCalculator::calculateScore(s),
                   -1,
                   0);
    }
    // hard 4: ifExists semantics — two reversed fixtures on the next index
    // still penalize the match only once.
    {
        LeagueSchedule s = makeBaseContiguous(3, 3);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 0, 1);
        addMatch(s, 1, 0, 1);
        // repeat: 1 (the round-0 match; ifExists counts once). sameDay: the
        // two identical round-1 matches pair up once. away run of team 0 at
        // indices {0? no — team 0 is home at 0} — home0@{0}, away1@{0},
        // home1@{1,1}, away0@{1,1}: 2 items < 4. No hops (zero distances).
        checkScore("repeat counted once per match",
                   ScoreCalculator::calculateScore(s),
                   -2,
                   0);
    }

    // soft 5: startToAwayHop — only matches on a round with no previous
    // round index pay dist(away, home).
    {
        LeagueSchedule s = makeBaseContiguous(4, 3);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 2, 1);
        // start: d(1,0) = 21 for the round-0 match only. No other hop fires
        // (no team-sharing across the consecutive rounds; round 2 exists so
        // no end hop).
        checkScore(
            "startToAwayHop", ScoreCalculator::calculateScore(s), 0, -21);
    }
    // soft 6: homeToAwayHop — m2.away == m1.home on consecutive indices pays
    // dist(m1.home, m2.home).
    {
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 0, 1);
        // start: d(1,0)=21. homeToAway: d(0,2)=13. end: d(2,0)=31.
        checkScore("homeToAwayHop (+start/end)",
                   ScoreCalculator::calculateScore(s),
                   0,
                   -65);
    }
    // soft 7: awayToAwayHop — m2.away == m1.away pays dist(m1.home, m2.home).
    {
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 1, 1);
        // start: d(1,0)=21. awayToAway: d(0,2)=13. end: d(2,1)=32.
        checkScore("awayToAwayHop (+start/end)",
                   ScoreCalculator::calculateScore(s),
                   0,
                   -66);
    }
    // soft 8: awayToHomeHop — m2.home == m1.away pays dist(m1.home, m1.away).
    {
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 2, 1);
        // start: d(1,0)=21. awayToHome: d(0,1)=12. end: d(1,2)=23.
        checkScore("awayToHomeHop (+start/end)",
                   ScoreCalculator::calculateScore(s),
                   0,
                   -56);
    }
    // soft 9: awayToEndHop — a single round is both first and last.
    {
        LeagueSchedule s = makeBaseContiguous(4, 1);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 0);  // unassigned: contributes nothing
        // start: d(1,0)=21; end: d(0,1)=12.
        checkScore("awayToEndHop on the only round",
                   ScoreCalculator::calculateScore(s),
                   0,
                   -33);
    }
    // Sparse round indices: rounds {0,1,4,6}; a match on index 4 has neither
    // a previous (3) nor a next (5) round, so it pays both boundary hops.
    {
        LeagueSchedule s = makeBase(4, {0, 1, 4, 6});
        useSyntheticDistances(s);
        addMatch(s, 0, 1, /*round position of index 4*/ 2);
        checkScore("sparse indices: both boundary hops",
                   ScoreCalculator::calculateScore(s),
                   0,
                   -33);
    }

    // soft 10: classic matches off weekends cost 1000 each (Java mirror:
    // two classic matches on plain rounds, one on a weekend round is free).
    {
        LeagueSchedule s = makeBaseContiguous(4, 5);
        s.rounds[1].weekendOrHoliday = true;
        addMatch(s, 0, 1, 0, /*classic*/ true);
        addMatch(s, 2, 3, 1, /*classic*/ true);  // weekend: free
        addMatch(s, 0, 1, 4, /*classic*/ true);
        checkScore("classic matches off weekend",
                   ScoreCalculator::calculateScore(s),
                   0,
                   -2000);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random mid-size instance for the incremental cross-check and the solves.
// -------------------------------------------------------------------------
LeagueSchedule buildRandomInstance(int matchCount, std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    LeagueSchedule s = makeBaseContiguous(/*teams*/ 6, /*rounds*/ 10);
    for (Round &round : s.rounds)
        round.weekendOrHoliday = (rng() % 3) == 0;
    const int n = s.teamCount();
    s.distanceToTeam.assign(n, std::vector<int>(n, 0));
    for (int a = 0; a < n; ++a)
    {
        for (int b = 0; b < n; ++b)
        {
            if (a != b)
                s.distanceToTeam[a][b] = static_cast<int>(rng() % 100);
        }
    }
    for (int i = 0; i < matchCount; ++i)
    {
        const int home = static_cast<int>(rng() % n);
        int away = static_cast<int>(rng() % (n - 1));
        if (away >= home)
            ++away;
        addMatch(s, home, away, /*round*/ -1, /*classic*/ (rng() % 5) == 0);
    }
    return s;
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves (including unassignments), driven through the ScoreDirector so
// the before/set/after hook contract is exercised exactly like in the engine.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Verifying incremental scoring == full recompute ==\n");
    LeagueSchedule s = buildRandomInstance(/*matchCount*/ 40, /*seed*/ 7);

    SportsLeagueIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<LeagueSchedule> director(s, calc);
    LeagueSchedule shadow = s;

    std::mt19937_64 rng(123);
    const int matchCount = s.matchCount();
    const int valueCount = s.roundCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    int costMismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            // ChangeMove; 10% of the time to unassigned (-1), like the
            // construction heuristic's starting state.
            const int match = static_cast<int>(rng() % matchCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(match, value);
        }
        else
        {
            // SwapMove.
            const int a = static_cast<int>(rng() % matchCount);
            const int b = static_cast<int>(rng() % matchCount);
            const int va = s.matches[a].round;
            const int vb = s.matches[b].round;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < matchCount; ++i)
            shadow.matches[i].round = s.matches[i].round;
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
        // The per-match guidance signal must track the same deltas: summed
        // over all matches it is the soft total with the three hop terms
        // counted twice (once per match of each pair).
        const Breakdown b = ScoreCalculator::computeBreakdown(shadow);
        const std::int64_t expectedCost = b.softTotal() + b.homeToAwayHop
                                          + b.awayToAwayHop + b.awayToHomeHop;
        std::int64_t actualCost = 0;
        for (int i = 0; i < matchCount; ++i)
            actualCost += calc.matchCost(i);
        if (actualCost != expectedCost && ++costMismatches <= 3)
        {
            std::printf("  COST MISMATCH @step %d: sum=%lld expected=%lld\n",
                        step,
                        static_cast<long long>(actualCost),
                        static_cast<long long>(expectedCost));
        }
    }
    check(mismatches == 0,
          std::to_string(steps)
              + " random moves, incremental == full recompute");
    check(costMismatches == 0,
          "per-match travel cost tracks the soft breakdown");
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
    LeagueSchedule s = buildRandomInstance(/*matchCount*/ 24, /*seed*/ 11);

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
// (d) A 6-team double round robin (30 matches, 12 rounds) reaches 0hard.
// -------------------------------------------------------------------------
void verifyFeasibleSolve()
{
    std::printf("== (d) Double round robin reaches 0hard ==\n");
    LeagueSchedule s = makeBaseContiguous(/*teams*/ 6, /*rounds*/ 12);
    useSyntheticDistances(s);
    for (int i = 0; i < 6; ++i)
    {
        for (int j = 0; j < 6; ++j)
        {
            if (i != j)
                addMatch(s, i, j);
        }
    }

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(5);
    config.randomSeed = 5;
    config.bestScoreLimit = Score{0, -1e18};  // stop at first feasible
    Solver solver(config);
    solver.solve(s);

    check(s.score.hard == 0, "double round robin reaches 0hard");
    bool allAssigned = true;
    for (const Match &match : s.matches)
    {
        if (match.round < 0)
            allAssigned = false;
    }
    check(allAssigned, "every match assigned after the solve");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifyFeasibleSolve();

    if (failures == 0)
    {
        std::printf("All sports-league-scheduling checks PASSED.\n");
        return 0;
    }
    std::printf("%d sports-league-scheduling check(s) FAILED.\n", failures);
    return 1;
}

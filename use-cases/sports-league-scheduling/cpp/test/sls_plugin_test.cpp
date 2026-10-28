// Plugin-assembly (sls/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (unassigned matches included)
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

#include "sls/constraints.hpp"
#include "sls/domain.hpp"
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
                long long expectedHard,
                double expectedSoft)
{
    const bool ok = (actual.hard == expectedHard
                     && std::fabs(actual.soft - expectedSoft) < 1e-9);
    std::printf("  [%s] %-46s got=%s expected=%lldhard/%gsoft\n",
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

Score scoreWith(DecomposedCalculator &calculator, LeagueSchedule &s)
{
    ScoreCalculator::prepare(s);  // idempotent; the components require it
    calculator.resetWorkingSolution(s);
    return calculator.calculateScore();
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    LeagueSchedule s = buildRandomInstance(/*matches*/ 30, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int mismatches = 0;
    const int rounds = 500;
    for (int round = 0; round < rounds; ++round)
    {
        for (Match &match : s.matches)
        {
            match.round = unit(rng) < 0.10
                              ? -1
                              : static_cast<int>(rng() % s.roundCount());
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
    LeagueSchedule s = buildRandomInstance(/*matches*/ 30, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<LeagueSchedule> director(s, calculator);
    LeagueSchedule shadow = s;

    std::mt19937_64 rng(123);
    const int matchCount = s.matchCount();
    const int valueCount = s.roundCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            const int match = static_cast<int>(rng() % matchCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(match, value);
        }
        else
        {
            const int a = static_cast<int>(rng() % matchCount);
            const int b = static_cast<int>(rng() % matchCount);
            const int va = s.matches[a].round;
            const int vb = s.matches[b].round;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        for (int i = 0; i < matchCount; ++i)
            shadow.matches[i].round = s.matches[i].round;
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

    {  // matchesOnSameDay alone: the same fixture twice in one round shares
        // BOTH teams and still counts once.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 1);  // unassigned: contributes nothing
        DecomposedCalculator calculator;
        calculator.emplace<MatchesOnSameDay>();
        checkScore("matchesOnSameDay alone", scoreWith(calculator, s), -1, 0.0);
    }
    {  // Disjoint teams in one round: silent.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 3, 0);
        DecomposedCalculator calculator;
        calculator.emplace<MatchesOnSameDay>();
        checkScore("matchesOnSameDay, disjoint teams",
                   scoreWith(calculator, s),
                   0,
                   0.0);
    }
    {  // consecutiveHomeMatches alone: team 0 at home on indices 0..3 is a
        // run of 4 -> penalized by its length.
        LeagueSchedule s = makeBaseContiguous(6, 5);
        for (int r = 0; r < 4; ++r)
            addMatch(s, 0, 1 + r, r);
        DecomposedCalculator calculator;
        calculator.emplace<ConsecutiveHomeMatches>();
        checkScore("consecutiveHomeMatches, run of 4",
                   scoreWith(calculator, s),
                   -4,
                   0.0);
    }
    {  // A run of 3 stays free.
        LeagueSchedule s = makeBaseContiguous(6, 5);
        for (int r = 0; r < 3; ++r)
            addMatch(s, 0, 1 + r, r);
        DecomposedCalculator calculator;
        calculator.emplace<ConsecutiveHomeMatches>();
        checkScore("consecutiveHomeMatches, run of 3 is free",
                   scoreWith(calculator, s),
                   0,
                   0.0);
    }
    {  // consecutiveAwayMatches alone: the away mirror.
        LeagueSchedule s = makeBaseContiguous(6, 5);
        for (int r = 0; r < 4; ++r)
            addMatch(s, 1 + r, 0, r);
        DecomposedCalculator calculator;
        calculator.emplace<ConsecutiveAwayMatches>();
        checkScore("consecutiveAwayMatches, run of 4",
                   scoreWith(calculator, s),
                   -4,
                   0.0);
    }
    {  // repeatMatchOnNextDay alone: the reversed fixture on the next index.
        LeagueSchedule s = makeBaseContiguous(4, 3);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 0, 1);
        DecomposedCalculator calculator;
        calculator.emplace<RepeatMatchOnNextDay>();
        checkScore(
            "repeatMatchOnNextDay alone", scoreWith(calculator, s), -1, 0.0);
    }
    {  // Two rounds apart: silent.
        LeagueSchedule s = makeBaseContiguous(4, 3);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 0, 2);
        DecomposedCalculator calculator;
        calculator.emplace<RepeatMatchOnNextDay>();
        checkScore("repeatMatchOnNextDay, two rounds apart",
                   scoreWith(calculator, s),
                   0,
                   0.0);
    }
    {  // classicMatches alone: a derby off a weekend round costs 1'000.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        s.rounds[1].weekendOrHoliday = true;
        addMatch(s, 0, 1, /*round*/ 0, /*classic*/ true);
        DecomposedCalculator calculator;
        calculator.emplace<ClassicMatches>();
        checkScore("classicMatches, derby on a weekday",
                   scoreWith(calculator, s),
                   0,
                   -1000.0);

        s.matches[0].round = 1;  // the weekend round
        DecomposedCalculator weekend;
        weekend.emplace<ClassicMatches>();
        checkScore("classicMatches, derby on a weekend",
                   scoreWith(weekend, s),
                   0,
                   0.0);
    }
    {  // startToAwayHop / awayToEndHop alone on a one-round season: the away
        // team travels in and back out.
        LeagueSchedule s = makeBaseContiguous(4, 1);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        DecomposedCalculator start;
        start.emplace<StartToAwayHop>();
        checkScore("startToAwayHop alone",
                   scoreWith(start, s),
                   0,
                   -static_cast<double>(s.distance(1, 0)));
        DecomposedCalculator end;
        end.emplace<AwayToEndHop>();
        checkScore("awayToEndHop alone",
                   scoreWith(end, s),
                   0,
                   -static_cast<double>(s.distance(0, 1)));
    }
    {  // homeToAwayHop alone: team 0 hosts on index 0, visits on index 1.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 0, 1);
        DecomposedCalculator calculator;
        calculator.emplace<HomeToAwayHop>();
        checkScore("homeToAwayHop alone",
                   scoreWith(calculator, s),
                   0,
                   -static_cast<double>(s.distance(0, 2)));
    }
    {  // awayToAwayHop alone: team 1 visits on both indices.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 1, 1);
        DecomposedCalculator calculator;
        calculator.emplace<AwayToAwayHop>();
        checkScore("awayToAwayHop alone",
                   scoreWith(calculator, s),
                   0,
                   -static_cast<double>(s.distance(0, 2)));
    }
    {  // awayToHomeHop alone: team 1 visits on index 0 and hosts on index 1;
        // the distance is the EARLIER match's own home-to-away hop.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<AwayToHomeHop>();
        checkScore("awayToHomeHop alone",
                   scoreWith(calculator, s),
                   0,
                   -static_cast<double>(s.distance(0, 1)));
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    LeagueSchedule s = makeBaseContiguous(4, 2);
    addMatch(s, 0, 1, 0);
    addMatch(s, 0, 1, 0);

    {
        DecomposedCalculator calculator;
        calculator.emplace<MatchesOnSameDay>(10.0);
        checkScore("matchesOnSameDay re-weighted to 10",
                   scoreWith(calculator, s),
                   -10,
                   0.0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<MatchesOnSameDay>();
        component->isHard = false;
        calculator.add(std::move(component));
        checkScore("matchesOnSameDay demoted to soft",
                   scoreWith(calculator, s),
                   0,
                   -1.0);
    }
    {
        // A third match in the round makes 3 pairs; exponent 2 shapes the raw
        // pair count before the weight.
        LeagueSchedule three = makeBaseContiguous(4, 2);
        addMatch(three, 0, 1, 0);
        addMatch(three, 0, 1, 0);
        addMatch(three, 0, 1, 0);
        DecomposedCalculator calculator;
        auto component = std::make_unique<MatchesOnSameDay>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("matchesOnSameDay with exponent 2",
                   scoreWith(calculator, three),
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
    LeagueSchedule s = buildRandomInstance(/*matches*/ 24, /*seed*/ 13);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(5);
    for (Match &match : s.matches)
        match.round = static_cast<int>(rng() % s.roundCount());

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
    check(calculator.constraintTotals().size() == 10,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    LeagueSchedule s = buildRandomInstance(/*matches*/ 20, /*seed*/ 11);

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
        std::printf("All sports-league-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d sports-league-scheduling plugin check(s) FAILED.\n",
                failures);
    return 1;
}

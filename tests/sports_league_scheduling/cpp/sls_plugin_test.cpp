// Plugin-assembly (sls/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedObjective
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (unassigned matches included)
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's Editor protocol (plugin path)
//   (c) partial assemblies: each component in isolation, hand-computed
//   (d) weight / level / exponent overrides
//   (e) constraint_totals() explains exactly the reported score
//   (f) a 2-second solve(schedule, calculator) under ScoreChecks::Full
//   (g) the defaultConstraints() builder: named edits (registry names) ==
//       the same assembly listed by hand
//   (h) each component's raw cost, hand-calculated: a positive and a zero
//       case per constraint (raw 0 = no violation), read through
//       constraint_costs()
//   (i) the catalog: info() off the default assembly == the components'
//       kDefaultWeight / kDefaultLevel; the knob guards
//   (j) repeated components and the offset / clip knobs: hand-calculated
//       repeats of a per-match unit rule and a global, threshold and literal
//       offsets by hand (in literal mode every round position counts, an
//       empty one included; a global shapes its one total), the literal
//       formula refused on hard at reset and kept on soft, incremental ==
//       a fresh recompute over a random walk and a ScoreChecks::Full solve with
//       repeated, shifted components
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

#include "sls/constraints.hpp"
#include "sls/domain.hpp"
#include "sls/score_calculator.hpp"
#include "sls/solver.hpp"
#include "optimaru/model/editor.hpp"

using namespace sls;

namespace {

int failures = 0;

void check(bool ok, const std::string& label) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}

void checkScore(const std::string& label, const Score& actual, long long expectedHard,
                double expectedSoft) {
    const bool ok = (actual.hard == expectedHard && std::fabs(actual.soft - expectedSoft) < 1e-9);
    std::printf("  [%s] %-46s got=%s expected=%lldhard/%gsoft\n", ok ? "PASS" : "FAIL",
                label.c_str(), actual.str().c_str(), expectedHard, expectedSoft);
    if (!ok) ++failures;
}

bool sameScore(const Score& a, const Score& b) {
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

LeagueSchedule makeBase(int teamCount, const std::vector<int>& roundIndices) {
    LeagueSchedule s;
    for (int t = 0; t < teamCount; ++t)
        s.teams.push_back({"T" + std::to_string(t), "Team " + std::to_string(t)});
    for (int index : roundIndices) {
        Round round;
        round.index = index;
        s.rounds.push_back(round);
    }
    return s;
}

LeagueSchedule makeBaseContiguous(int teamCount, int roundCount) {
    std::vector<int> indices(roundCount);
    for (int i = 0; i < roundCount; ++i) indices[i] = i;
    return makeBase(teamCount, indices);
}

void useSyntheticDistances(LeagueSchedule& s) {
    const int n = s.teamCount();
    s.distanceToTeam.assign(n, std::vector<int>(n, 0));
    for (int a = 0; a < n; ++a) {
        for (int b = 0; b < n; ++b) {
            if (a != b) s.distanceToTeam[a][b] = 10 * (a + 1) + (b + 1);
        }
    }
}

void addMatch(LeagueSchedule& s, int homeTeam, int awayTeam, int round = -1, bool classic = false) {
    Match match;
    match.id = "M" + std::to_string(s.matches.size());
    match.homeTeam = homeTeam;
    match.awayTeam = awayTeam;
    match.round = round;
    match.classicMatch = classic;
    s.matches.push_back(match);
}

LeagueSchedule buildRandomInstance(int matchCount, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    LeagueSchedule s = makeBaseContiguous(/*teams*/ 6, /*rounds*/ 10);
    for (Round& round : s.rounds) round.weekendOrHoliday = (rng() % 3) == 0;
    const int n = s.teamCount();
    s.distanceToTeam.assign(n, std::vector<int>(n, 0));
    for (int a = 0; a < n; ++a) {
        for (int b = 0; b < n; ++b) {
            if (a != b) s.distanceToTeam[a][b] = static_cast<int>(rng() % 100);
        }
    }
    for (int i = 0; i < matchCount; ++i) {
        const int home = static_cast<int>(rng() % n);
        int away = static_cast<int>(rng() % (n - 1));
        if (away >= home) ++away;
        addMatch(s, home, away, /*round*/ -1, /*classic*/ (rng() % 5) == 0);
    }
    return s;
}

Score scoreWith(DecomposedCalculator& calculator, LeagueSchedule& s) {
    ScoreCalculator::prepare(s);  // idempotent; the components require it
    calculator.rebuild(s);
    return calculator.score();
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity() {
    std::printf("== (a) Default assembly == monolithic score ==\n");
    LeagueSchedule s = buildRandomInstance(/*matches*/ 30, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int mismatches = 0;
    const int rounds = 500;
    for (int round = 0; round < rounds; ++round) {
        for (Match& match : s.matches) {
            match.round = unit(rng) < 0.10 ? -1 : static_cast<int>(rng() % s.roundCount());
        }
        const Score plugin = scoreWith(calculator, s);
        const Score monolithic = ScoreCalculator::score(s);
        if (!sameScore(plugin, monolithic)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n", round,
                            plugin.str().c_str(), monolithic.str().c_str());
            }
        }
    }
    check(mismatches == 0, std::to_string(rounds) + " random assignments score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk.
// -------------------------------------------------------------------------
void verifyIncremental() {
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    LeagueSchedule s = buildRandomInstance(/*matches*/ 30, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::Editor<LeagueSchedule> editor(s, calculator);
    LeagueSchedule shadow = s;

    std::mt19937_64 rng(123);
    const int matchCount = s.matchCount();
    const int value_count = s.roundCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step) {
        if (unit(rng) < 0.75) {
            const int match = static_cast<int>(rng() % matchCount);
            const int value = (unit(rng) < 0.10) ? -1 : static_cast<int>(rng() % value_count);
            editor.assign(match, value);
        } else {
            const int a = static_cast<int>(rng() % matchCount);
            const int b = static_cast<int>(rng() % matchCount);
            const int va = s.matches[a].round;
            const int vb = s.matches[b].round;
            editor.assign(a, vb);
            editor.assign(b, va);
        }
        for (int i = 0; i < matchCount; ++i) shadow.matches[i].round = s.matches[i].round;
        const Score full = ScoreCalculator::score(shadow);
        const Score incremental = editor.score();
        if (!sameScore(full, incremental)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n", step,
                            incremental.str().c_str(), full.str().c_str());
            }
        }
    }
    check(mismatches == 0, std::to_string(steps) + " random moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Single-component assemblies on hand-computed instances.
// -------------------------------------------------------------------------
void verifyPartialAssemblies() {
    std::printf("== (c) Single-component assemblies ==\n");

    {  // teamConflictCost alone: the same fixture twice in one round shares
        // BOTH teams and still counts once.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 1);  // unassigned: contributes nothing
        DecomposedCalculator calculator;
        calculator.emplace<TeamConflictCost>();
        checkScore("teamConflictCost alone", scoreWith(calculator, s), -1, 0.0);
    }
    {  // Disjoint teams in one round: silent.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 3, 0);
        DecomposedCalculator calculator;
        calculator.emplace<TeamConflictCost>();
        checkScore("teamConflictCost, disjoint teams", scoreWith(calculator, s), 0, 0.0);
    }
    {  // consecutiveHomeMatchesCost alone: team 0 at home on indices 0..3 is a
        // run of 4 -> penalized by its length.
        LeagueSchedule s = makeBaseContiguous(6, 5);
        for (int r = 0; r < 4; ++r) addMatch(s, 0, 1 + r, r);
        DecomposedCalculator calculator;
        calculator.emplace<ConsecutiveHomeMatchesCost>();
        checkScore("consecutiveHomeMatchesCost, run of 4", scoreWith(calculator, s), -4, 0.0);
    }
    {  // A run of 3 stays free.
        LeagueSchedule s = makeBaseContiguous(6, 5);
        for (int r = 0; r < 3; ++r) addMatch(s, 0, 1 + r, r);
        DecomposedCalculator calculator;
        calculator.emplace<ConsecutiveHomeMatchesCost>();
        checkScore("consecutiveHomeMatchesCost, run of 3 is free", scoreWith(calculator, s), 0,
                   0.0);
    }
    {  // consecutiveAwayMatchesCost alone: the away mirror.
        LeagueSchedule s = makeBaseContiguous(6, 5);
        for (int r = 0; r < 4; ++r) addMatch(s, 1 + r, 0, r);
        DecomposedCalculator calculator;
        calculator.emplace<ConsecutiveAwayMatchesCost>();
        checkScore("consecutiveAwayMatchesCost, run of 4", scoreWith(calculator, s), -4, 0.0);
    }
    {  // repeatMatchOnNextDayCost alone: the reversed fixture on the next
        // index.
        LeagueSchedule s = makeBaseContiguous(4, 3);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 0, 1);
        DecomposedCalculator calculator;
        calculator.emplace<RepeatMatchOnNextDayCost>();
        checkScore("repeatMatchOnNextDayCost alone", scoreWith(calculator, s), -1, 0.0);
    }
    {  // Two rounds apart: silent.
        LeagueSchedule s = makeBaseContiguous(4, 3);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 0, 2);
        DecomposedCalculator calculator;
        calculator.emplace<RepeatMatchOnNextDayCost>();
        checkScore("repeatMatchOnNextDayCost, two rounds apart", scoreWith(calculator, s), 0, 0.0);
    }
    {  // workdayClassicMatchesCost alone: a derby off a weekend round costs
        // 1'000.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        s.rounds[1].weekendOrHoliday = true;
        addMatch(s, 0, 1, /*round*/ 0, /*classic*/ true);
        DecomposedCalculator calculator;
        calculator.emplace<WorkdayClassicMatchesCost>();
        checkScore("workdayClassicMatchesCost, derby on a weekday", scoreWith(calculator, s), 0,
                   -1000.0);

        s.matches[0].round = 1;  // the weekend round
        DecomposedCalculator weekend;
        weekend.emplace<WorkdayClassicMatchesCost>();
        checkScore("workdayClassicMatchesCost, derby on a weekend", scoreWith(weekend, s), 0, 0.0);
    }
    {  // startToAwayHopCost / awayToEndHopCost alone on a one-round season:
        // the away team travels in and back out.
        LeagueSchedule s = makeBaseContiguous(4, 1);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        DecomposedCalculator start;
        start.emplace<StartToAwayHopCost>();
        checkScore("startToAwayHopCost alone", scoreWith(start, s), 0,
                   -static_cast<double>(s.distance(1, 0)));
        DecomposedCalculator end;
        end.emplace<AwayToEndHopCost>();
        checkScore("awayToEndHopCost alone", scoreWith(end, s), 0,
                   -static_cast<double>(s.distance(0, 1)));
    }
    {  // homeToAwayHopCost alone: team 0 hosts on index 0, visits on index 1.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 0, 1);
        DecomposedCalculator calculator;
        calculator.emplace<HomeToAwayHopCost>();
        checkScore("homeToAwayHopCost alone", scoreWith(calculator, s), 0,
                   -static_cast<double>(s.distance(0, 2)));
    }
    {  // awayToAwayHopCost alone: team 1 visits on both indices.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 1, 1);
        DecomposedCalculator calculator;
        calculator.emplace<AwayToAwayHopCost>();
        checkScore("awayToAwayHopCost alone", scoreWith(calculator, s), 0,
                   -static_cast<double>(s.distance(0, 2)));
    }
    {  // awayToHomeHopCost alone: team 1 visits on index 0 and hosts on index
        // 1; the distance is the EARLIER match's own home-to-away hop.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<AwayToHomeHopCost>();
        checkScore("awayToHomeHopCost alone", scoreWith(calculator, s), 0,
                   -static_cast<double>(s.distance(0, 1)));
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides() {
    std::printf("== (d) Weight / level overrides ==\n");
    LeagueSchedule s = makeBaseContiguous(4, 2);
    addMatch(s, 0, 1, 0);
    addMatch(s, 0, 1, 0);

    {
        DecomposedCalculator calculator;
        calculator.emplace<TeamConflictCost>(10.0);
        checkScore("teamConflictCost re-weighted to 10", scoreWith(calculator, s), -10, 0.0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<TeamConflictCost>();
        component->level = optimaru::Level::Soft;
        calculator.add(std::move(component));
        checkScore("teamConflictCost demoted to soft", scoreWith(calculator, s), 0, -1.0);
    }
    {
        // A third match in the round makes 3 pairs; exponent 2 shapes the raw
        // pair count before the weight.
        LeagueSchedule three = makeBaseContiguous(4, 2);
        addMatch(three, 0, 1, 0);
        addMatch(three, 0, 1, 0);
        addMatch(three, 0, 1, 0);
        DecomposedCalculator calculator;
        auto component = std::make_unique<TeamConflictCost>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("teamConflictCost with exponent 2", scoreWith(calculator, three), -9, 0.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraint_totals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals() {
    std::printf("== (e) constraintTotals() explains the score ==\n");
    LeagueSchedule s = buildRandomInstance(/*matches*/ 24, /*seed*/ 13);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(5);
    for (Match& match : s.matches) match.round = static_cast<int>(rng() % s.roundCount());

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    const Score score = scoreWith(calculator, s);

    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal& total : calculator.constraint_totals())
        (total.is_hard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9 &&
                  std::fabs(-soft - score.soft) < 1e-6,
          "per-constraint totals sum to the reported score");
    check(calculator.constraint_totals().size() == 10, "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) ScoreChecks::Full solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve() {
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    LeagueSchedule s = buildRandomInstance(/*matches*/ 20, /*seed*/ 11);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(2);
    config.seed = 3;
    config.score_checks = optimaru::ScoreChecks::Full;
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    bool corrupted = false;
    try {
        Solver solver(config);
        solver.solve(s, calculator);
    } catch (const optimaru::ScoreMismatchError& e) {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted) {
        const Score recomputed = ScoreCalculator::score(s);
        check(sameScore(s.score, recomputed), "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) The defaultConstraints() builder, by registry name.
// -------------------------------------------------------------------------
bool throwsInvalid(const std::function<void()>& edit) {
    try {
        edit();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void verifyAssemblyBuilder() {
    std::printf("== (g) defaultConstraints() named edits ==\n");
    LeagueSchedule s = buildRandomInstance(/*matches*/ 24, /*seed*/ 21);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(17);
    for (Match& match : s.matches) match.round = static_cast<int>(rng() % s.roundCount());

    // By hand: the default set with teamConflictCost demoted to soft, the
    // derby weight doubled and awayToEndHopCost left out.
    DecomposedCalculator byHand;
    {
        auto teamConflict = std::make_unique<TeamConflictCost>();
        teamConflict->level = optimaru::Level::Soft;
        byHand.add(std::move(teamConflict));
        byHand.emplace<ConsecutiveHomeMatchesCost>();
        byHand.emplace<ConsecutiveAwayMatchesCost>();
        byHand.emplace<RepeatMatchOnNextDayCost>();
        byHand.emplace<StartToAwayHopCost>();
        byHand.emplace<HomeToAwayHopCost>();
        byHand.emplace<AwayToAwayHopCost>();
        byHand.emplace<AwayToHomeHopCost>();
        byHand.emplace<WorkdayClassicMatchesCost>(2.0 * WorkdayClassicMatchesCost::kDefaultWeight);
    }
    const Score expected = scoreWith(byHand, s);

    // By name: the same three edits, one expression.
    DecomposedCalculator built;
    defaultConstraints()
            .soft("teamConflictCost")
            .weight("workdayClassicMatchesCost", 2.0 * WorkdayClassicMatchesCost::kDefaultWeight)
            .drop("awayToEndHopCost")
            .into(built);
    check(sameScore(scoreWith(built, s), expected),
          "named edits == the same assembly listed by hand");

    // The default assembly, untouched, still matches the monolith.
    DecomposedCalculator plain;
    defaultConstraints().into(plain);
    check(sameScore(scoreWith(plain, s), ScoreCalculator::score(s)),
          "defaultConstraints() alone == the monolithic score");

    // A name without the Cost suffix is unknown: a typo must not quietly
    // leave the cost function as it was.
    check(throwsInvalid([] { defaultConstraints().weight("workdayClassicMatches", 2); }),
          "a registry name without its Cost suffix is unknown (throws)");

    // only() isolates a subset: just the hard rule, nothing else scored.
    DecomposedCalculator isolated;
    defaultConstraints().only({"teamConflictCost"}).into(isolated);
    const Score hardOnly = scoreWith(isolated, s);
    check(hardOnly.soft == 0.0 && hardOnly.hard != 0, "only() scores exactly the named constraint");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) Each component's raw cost on hand-built instances: one violating
// (positive) case and one clean (zero) case per constraint, read through
// constraint_costs() — the raw numbers before weight, exponent and level. The
// three unit constraints report one cost per round position, the seven
// globals one total. Distances are the synthetic
// dist(a, b) = 10 * (a + 1) + (b + 1).
// -------------------------------------------------------------------------
std::vector<double> rawCosts(LeagueSchedule& schedule, const std::string& name) {
    ScoreCalculator::prepare(schedule);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(schedule);
    calculator.score();
    for (const auto& entry : calculator.constraint_costs()) {
        if (entry.name == name) return entry.unit_costs;
    }
    return {};
}

void checkRaw(LeagueSchedule& schedule, const std::string& name,
              const std::vector<double>& expected, const std::string& label) {
    check(rawCosts(schedule, name) == expected, name + ": " + label);
}

void verifyRawCostsByHand() {
    std::printf("== (h) Raw costs by hand, positive and zero ==\n");
    {  // (0,1), (0,1), (1,2) on one index: every pair shares a team, the
        // first pair shares both and still counts once -> 3 pairs.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 2, 0);
        checkRaw(s, "teamConflictCost", {3.0}, "{3} (three sharing pairs)");
    }
    {
        LeagueSchedule s = makeBaseContiguous(4, 2);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 3, 0);
        checkRaw(s, "teamConflictCost", {0.0}, "{0} (disjoint teams)");
    }
    {  // Team 0 at home on indices 0..4: one run of 5, charged in full.
        LeagueSchedule s = makeBaseContiguous(4, 5);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 2, 1);
        addMatch(s, 0, 3, 2);
        addMatch(s, 0, 1, 3);
        addMatch(s, 0, 2, 4);
        checkRaw(s, "consecutiveHomeMatchesCost", {5.0}, "{5} (the whole run of 5)");
    }
    {  // A run of 3 is free.
        LeagueSchedule s = makeBaseContiguous(4, 5);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 2, 1);
        addMatch(s, 0, 3, 2);
        checkRaw(s, "consecutiveHomeMatchesCost", {0.0}, "{0} (run of 3)");
    }
    {  // Team 0 away on indices 0..3: one run of 4.
        LeagueSchedule s = makeBaseContiguous(4, 5);
        addMatch(s, 1, 0, 0);
        addMatch(s, 2, 0, 1);
        addMatch(s, 3, 0, 2);
        addMatch(s, 1, 0, 3);
        checkRaw(s, "consecutiveAwayMatchesCost", {4.0}, "{4} (run of 4)");
    }
    {  // Away on 0, 1, 3, 4: the gap on index 2 splits it into two runs of 2.
        LeagueSchedule s = makeBaseContiguous(4, 5);
        addMatch(s, 1, 0, 0);
        addMatch(s, 2, 0, 1);
        addMatch(s, 3, 0, 3);
        addMatch(s, 1, 0, 4);
        checkRaw(s, "consecutiveAwayMatchesCost", {0.0}, "{0} (split by a gap)");
    }
    {  // (0,1) on 0 is followed by TWO reversed fixtures on 1: once.
        LeagueSchedule s = makeBaseContiguous(3, 3);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 0, 1);
        addMatch(s, 1, 0, 1);
        checkRaw(s, "repeatMatchOnNextDayCost", {1.0}, "{1} (once per match)");
    }
    {  // The reversed fixture two indices later.
        LeagueSchedule s = makeBaseContiguous(3, 3);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 0, 2);
        checkRaw(s, "repeatMatchOnNextDayCost", {0.0}, "{0} (two apart)");
    }
    {  // Two matches on the first index: d(1,0) + d(3,2) = 21 + 43; the
        // match on index 1 has a previous index.
        LeagueSchedule s = makeBaseContiguous(4, 3);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 3, 0);
        addMatch(s, 0, 2, 1);
        checkRaw(s, "startToAwayHopCost", {64.0, 0.0, 0.0}, "{64, 0, 0} (two matches on index 0)");
    }
    {  // Nothing on the first index (it exists, so index 1 has a previous).
        LeagueSchedule s = makeBaseContiguous(4, 3);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 1);
        addMatch(s, 2, 3, 2);
        checkRaw(s, "startToAwayHopCost", {0.0, 0.0, 0.0}, "{0, 0, 0} (index 0 empty)");
    }
    {  // One match on the last index: d(0,1) = 12.
        LeagueSchedule s = makeBaseContiguous(4, 3);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 2);
        checkRaw(s, "awayToEndHopCost", {0.0, 0.0, 12.0}, "{0, 0, 12} (match on the last index)");
    }
    {
        LeagueSchedule s = makeBaseContiguous(4, 3);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        checkRaw(s, "awayToEndHopCost", {0.0, 0.0, 0.0}, "{0, 0, 0} (last index empty)");
    }
    {  // Two derbies on the weekday round 0, one on the weekend round 1.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        s.rounds[1].weekendOrHoliday = true;
        addMatch(s, 0, 1, 0, /*classic*/ true);
        addMatch(s, 2, 3, 0, /*classic*/ true);
        addMatch(s, 1, 0, 1, /*classic*/ true);
        checkRaw(s, "workdayClassicMatchesCost", {2.0, 0.0}, "{2, 0} (two workday derbies)");
    }
    {  // The derby on the weekend, a plain match on the weekday.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        s.rounds[1].weekendOrHoliday = true;
        addMatch(s, 0, 1, 1, /*classic*/ true);
        addMatch(s, 2, 3, 0, /*classic*/ false);
        checkRaw(s, "workdayClassicMatchesCost", {0.0, 0.0}, "{0, 0} (derby on the weekend)");
    }
    {  // Team 0 hosts on 0, visits team 2 on 1: d(0,2) = 13.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 0, 1);
        checkRaw(s, "homeToAwayHopCost", {13.0}, "{13} (home, then away)");
    }
    {  // Team 0 hosts on both indices.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 0, 2, 1);
        checkRaw(s, "homeToAwayHopCost", {0.0}, "{0} (home twice)");
    }
    {  // Team 1 visits 0, then 2: d(0,2) = 13.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 1, 1);
        checkRaw(s, "awayToAwayHopCost", {13.0}, "{13} (away twice)");
    }
    {  // Team 1 visits, then hosts: no away-to-away hop.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 2, 1);
        checkRaw(s, "awayToAwayHopCost", {0.0}, "{0} (away, then home)");
    }
    {  // Team 1 visits 0, then hosts: the earlier match's d(0,1) = 12.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 1, 2, 1);
        checkRaw(s, "awayToHomeHopCost", {12.0}, "{12} (away, then home)");
    }
    {  // Team 1 visits twice: no away-to-home hop.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        addMatch(s, 0, 1, 0);
        addMatch(s, 2, 1, 1);
        checkRaw(s, "awayToHomeHopCost", {0.0}, "{0} (away twice)");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The catalog read off the default assembly is exactly the components'
// constants, and the knob guards hold on the domain's assembly.
// -------------------------------------------------------------------------
template <typename Component>
bool rowIs(const optimaru::ConstraintInfo& row, const char* name, optimaru::Level level,
           double weight, bool is_global) {
    return row.name == name && row.default_level == Component::kDefaultLevel &&
           row.default_weight == Component::kDefaultWeight && row.default_level == level &&
           row.default_weight == weight && row.level_flippable && !row.reward &&
           row.is_global == is_global && row.default_enabled;
}

void verifyCatalog() {
    std::printf("== (i) Catalog and guards ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    // Assembly order: the three unit constraints first, then the globals.
    check(rows.size() == 10 &&
                  rowIs<StartToAwayHopCost>(rows[0], "startToAwayHopCost", Level::Soft, 1.0,
                                            false) &&
                  rowIs<AwayToEndHopCost>(rows[1], "awayToEndHopCost", Level::Soft, 1.0, false) &&
                  rowIs<WorkdayClassicMatchesCost>(rows[2], "workdayClassicMatchesCost",
                                                   Level::Soft, 1000.0, false) &&
                  rowIs<TeamConflictCost>(rows[3], "teamConflictCost", Level::Hard, 1.0, true) &&
                  rowIs<ConsecutiveHomeMatchesCost>(rows[4], "consecutiveHomeMatchesCost",
                                                    Level::Hard, 1.0, true) &&
                  rowIs<ConsecutiveAwayMatchesCost>(rows[5], "consecutiveAwayMatchesCost",
                                                    Level::Hard, 1.0, true) &&
                  rowIs<RepeatMatchOnNextDayCost>(rows[6], "repeatMatchOnNextDayCost", Level::Hard,
                                                  1.0, true) &&
                  rowIs<HomeToAwayHopCost>(rows[7], "homeToAwayHopCost", Level::Soft, 1.0, true) &&
                  rowIs<AwayToAwayHopCost>(rows[8], "awayToAwayHopCost", Level::Soft, 1.0, true) &&
                  rowIs<AwayToHomeHopCost>(rows[9], "awayToHomeHopCost", Level::Soft, 1.0, true),
          "info() == the components' constants (10 rows, assembly order)");

    check(throwsInvalid([] { defaultConstraints().weight("teamConflictCost", -1); }),
          "a negative weight is refused");
    check(throwsInvalid([] { defaultConstraints().exponent("awayToEndHopCost", 0); }),
          "a zero exponent is refused");

    // Every constraint of this domain flips both ways: the derby rule
    // promoted to hard keeps its weight in hard units (1 derby -> -1000
    // hard), and the hard rule demoted to soft costs 1 soft per pair.
    LeagueSchedule s = makeBaseContiguous(4, 2);
    addMatch(s, 0, 1, 0, /*classic*/ true);
    addMatch(s, 0, 2, 0);
    DecomposedCalculator flipped;
    defaultConstraints()
            .hard("workdayClassicMatchesCost")
            .soft("teamConflictCost")
            .only({"workdayClassicMatchesCost", "teamConflictCost"})
            .into(flipped);
    checkScore("derby rule hard, team conflict soft", scoreWith(flipped, s), -1000, -1.0);
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with add_instances(), as the Python bindings
// build it from a pick list.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string& name) {
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// Four teams at the synthetic distances, round positions 0 and 1:
// (0, 1) in round 0 and (2, 0) in round 1. Raw: startToAwayHopCost
// [d(1, 0) = 21, 0], awayToEndHopCost [0, d(2, 0) = 31],
// workdayClassicMatchesCost [0, 0]; homeToAwayHopCost totals d(0, 2) = 13,
// every other global 0.
LeagueSchedule repeatInstance() {
    LeagueSchedule s = makeBaseContiguous(4, 2);
    useSyntheticDistances(s);
    addMatch(s, 0, 1, /*round*/ 0);
    addMatch(s, 2, 0, /*round*/ 1);
    ScoreCalculator::prepare(s);
    return s;
}

// The defaults, two of them shifted (team conflict by a threshold: hard
// refuses the literal formula with an offset), plus repeats of a per-match
// delta rule (literal, fractional weight), a round-index pair global
// (threshold, squared), a team-side run global (negative offset), the
// clamped repeat rule and the derby rule (literal), and two repeats on the
// other level than their default: team conflict demoted to soft (literal,
// fractional weight) and the away-to-end hop promoted to hard (threshold).
ConstraintAssembly repeatedAssembly() {
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("teamConflictCost", 1.0);
    assembly.offset("awayToAwayHopCost", 50.0);
    assembly.add_instances(std::move(copyOf("startToAwayHopCost")
                                             .weight("startToAwayHopCost", 0.5)
                                             .offset("startToAwayHopCost", 40.0)
                                             .clip("startToAwayHopCost", false)));
    assembly.add_instances(std::move(copyOf("homeToAwayHopCost")
                                             .weight("homeToAwayHopCost", 0.25)
                                             .exponent("homeToAwayHopCost", 2.0)
                                             .offset("homeToAwayHopCost", 30.0)));
    assembly.add_instances(std::move(
            copyOf("consecutiveHomeMatchesCost").offset("consecutiveHomeMatchesCost", -1.0)));
    assembly.add_instances(std::move(copyOf("repeatMatchOnNextDayCost")));
    assembly.add_instances(std::move(copyOf("workdayClassicMatchesCost")
                                             .offset("workdayClassicMatchesCost", 1.0)
                                             .clip("workdayClassicMatchesCost", false)));
    assembly.add_instances(std::move(copyOf("teamConflictCost")
                                             .soft("teamConflictCost")
                                             .weight("teamConflictCost", 1.5)
                                             .offset("teamConflictCost", 1.0)
                                             .clip("teamConflictCost", false)));
    assembly.add_instances(std::move(copyOf("awayToEndHopCost")
                                             .hard("awayToEndHopCost")
                                             .weight("awayToEndHopCost", 2.0)
                                             .offset("awayToEndHopCost", 25.0)));
    return assembly;
}

void verifyRepeatsAndOffsets() {
    std::printf("== (j) Repeated components, offset / clip ==\n");
    {
        LeagueSchedule s = repeatInstance();
        ConstraintAssembly assembly = copyOf("homeToAwayHopCost");
        assembly.add_instances(std::move(copyOf("homeToAwayHopCost")
                                                 .weight("homeToAwayHopCost", 2.0)
                                                 .exponent("homeToAwayHopCost", 2.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        checkScore("home-to-away km + 2 x km^2 on the total: -(13 + 338)", scoreWith(calculator, s),
                   0, -351.0);
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].unit_costs == entries[1].unit_costs &&
                      entries[0].unit_costs.size() == 1 && entries[1].weight == 2.0 &&
                      entries[1].exponent == 2.0 && entries[1].default_weight == 1.0,
              "two global instances, the same total, each with its own knobs");
    }
    {
        LeagueSchedule s = repeatInstance();
        ConstraintAssembly assembly = copyOf("startToAwayHopCost");
        assembly.add_instances(std::move(copyOf("startToAwayHopCost")
                                                 .weight("startToAwayHopCost", 0.5)
                                                 .exponent("startToAwayHopCost", 2.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        checkScore("start hop + 0.5 x start hop^2 per round: -(21 + 220.5)",
                   scoreWith(calculator, s), 0, -241.5);
    }
    {
        // Each instance on its own level: the soft default home-to-away hop
        // (13) next to a hard repeat, 2 x (13 - 3) beyond its offset.
        LeagueSchedule s = repeatInstance();
        ConstraintAssembly assembly = copyOf("homeToAwayHopCost");
        assembly.add_instances(std::move(copyOf("homeToAwayHopCost")
                                                 .hard("homeToAwayHopCost")
                                                 .weight("homeToAwayHopCost", 2.0)
                                                 .offset("homeToAwayHopCost", 3.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        checkScore("soft home-to-away + a hard repeat beyond 3: -20hard", scoreWith(calculator, s),
                   -20, -13.0);
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].level == optimaru::Level::Soft &&
                      entries[1].level == optimaru::Level::Hard &&
                      entries[1].default_level == optimaru::Level::Soft,
              "the repeat reports its own level, the default stays soft");
    }
    {
        // A soft per-round rule with a hard threshold repeat (hard refuses
        // the literal formula with an offset): 3 x (21 - 1) on round 0, 0 on
        // round 1: -60 hard.
        LeagueSchedule s = repeatInstance();
        ConstraintAssembly assembly = copyOf("startToAwayHopCost");
        assembly.add_instances(std::move(copyOf("startToAwayHopCost")
                                                 .hard("startToAwayHopCost")
                                                 .weight("startToAwayHopCost", 3.0)
                                                 .offset("startToAwayHopCost", 1.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        checkScore("soft start hop + a hard threshold repeat: -60hard", scoreWith(calculator, s),
                   -60, -21.0);
    }
    {
        LeagueSchedule s = repeatInstance();
        DecomposedCalculator threshold;
        copyOf("startToAwayHopCost").offset("startToAwayHopCost", 10.0).into(threshold);
        checkScore("threshold offset 10: max(0, 21 - 10)", scoreWith(threshold, s), 0, -11.0);
        // Literal: round 1 (raw 0) is rewarded 10.
        DecomposedCalculator literal;
        copyOf("startToAwayHopCost")
                .offset("startToAwayHopCost", 10.0)
                .clip("startToAwayHopCost", false)
                .into(literal);
        checkScore("literal offset 10: (21 - 10) + (0 - 10), every round", scoreWith(literal, s), 0,
                   -1.0);
        DecomposedCalculator negative;
        copyOf("awayToEndHopCost").offset("awayToEndHopCost", -5.0).into(negative);
        checkScore("threshold offset -5 adds to the non-zero round only", scoreWith(negative, s), 0,
                   -36.0);
        DecomposedCalculator negativeLiteral;
        copyOf("awayToEndHopCost")
                .offset("awayToEndHopCost", -5.0)
                .clip("awayToEndHopCost", false)
                .into(negativeLiteral);
        checkScore("literal offset -5 adds to every round: 5 + 36", scoreWith(negativeLiteral, s),
                   0, -41.0);
        // A hard threshold offset rounds per round: (21 - 0.5) x 3 = 61.5 ->
        // 62, and the empty round stays 0.
        DecomposedCalculator hard;
        copyOf("startToAwayHopCost")
                .hard("startToAwayHopCost")
                .weight("startToAwayHopCost", 3.0)
                .offset("startToAwayHopCost", 0.5)
                .into(hard);
        checkScore("hard threshold offset 0.5 at weight 3, rounded per round", scoreWith(hard, s),
                   -62, 0.0);
        // The literal pick on soft is not rounded: 61.5 + (0 - 0.5) x 3.
        DecomposedCalculator soft;
        copyOf("startToAwayHopCost")
                .weight("startToAwayHopCost", 3.0)
                .offset("startToAwayHopCost", 0.5)
                .clip("startToAwayHopCost", false)
                .into(soft);
        checkScore("soft literal offset 0.5 at weight 3: -(61.5 - 1.5)", scoreWith(soft, s), 0,
                   -60.0);
        // A global shapes its one total.
        DecomposedCalculator globalThreshold;
        copyOf("homeToAwayHopCost").offset("homeToAwayHopCost", 3.0).into(globalThreshold);
        checkScore("home-to-away threshold offset 3: 13 - 3", scoreWith(globalThreshold, s), 0,
                   -10.0);
        DecomposedCalculator globalLiteral;
        copyOf("homeToAwayHopCost")
                .offset("homeToAwayHopCost", 20.0)
                .clip("homeToAwayHopCost", false)
                .into(globalLiteral);
        checkScore("home-to-away literal offset 20: 13 - 20, a reward", scoreWith(globalLiteral, s),
                   0, 7.0);
        // On soft, the literal team conflict rewards a 0 total; on hard (its
        // default) that would be a hard reward, which the reset refuses.
        DecomposedCalculator zeroSoft;
        copyOf("teamConflictCost")
                .soft("teamConflictCost")
                .offset("teamConflictCost", 1.0)
                .clip("teamConflictCost", false)
                .into(zeroSoft);
        checkScore("soft team conflict literal offset 1 on a 0 total: +1soft",
                   scoreWith(zeroSoft, s), 0, 1.0);
    }
    {
        // On hard the literal formula with an offset is refused at reset,
        // whichever sign and setter order, for a hard default global and for
        // a soft per-round rule moved to hard: +1 would reward a schedule
        // without team conflicts (+1 hard, cancelling another violation), -1
        // would charge a schedule that holds none.
        auto refusal = [](const std::string& name) {
            return "Constraint '" + name +
                   "' cannot use clip=false with a non-zero offset on a level "
                   "that counts toward the hard score (hard 0 must mean no "
                   "violation)";
        };
        auto resetRefusal = [](ConstraintAssembly assembly) {
            LeagueSchedule s = repeatInstance();
            DecomposedCalculator calculator;
            assembly.into(calculator);
            try {
                scoreWith(calculator, s);
            } catch (const std::logic_error& error) {
                return std::string(error.what());
            }
            return std::string();
        };
        check(resetRefusal(copyOf("teamConflictCost")
                                   .offset("teamConflictCost", 1.0)
                                   .clip("teamConflictCost", false)) ==
                              refusal("teamConflictCost") &&
                      resetRefusal(copyOf("teamConflictCost")
                                           .clip("teamConflictCost", false)
                                           .offset("teamConflictCost", -1.0)) ==
                              refusal("teamConflictCost"),
              "team conflict literal offset +-1 on hard: refused at reset");
        check(resetRefusal(copyOf("consecutiveAwayMatchesCost")
                                   .offset("consecutiveAwayMatchesCost", 2.0)
                                   .clip("consecutiveAwayMatchesCost", false)) ==
                      refusal("consecutiveAwayMatchesCost"),
              "consecutive away literal offset on hard: refused at reset");
        check(resetRefusal(copyOf("startToAwayHopCost")
                                   .offset("startToAwayHopCost", 10.0)
                                   .clip("startToAwayHopCost", false)
                                   .hard("startToAwayHopCost")) == refusal("startToAwayHopCost"),
              "start hop literal offset moved to hard afterwards: refused at "
              "reset");
        ConstraintAssembly repeated = copyOf("teamConflictCost");
        repeated.add_instances(std::move(copyOf("teamConflictCost")
                                                 .offset("teamConflictCost", 1.0)
                                                 .clip("teamConflictCost", false)));
        check(resetRefusal(std::move(repeated)) == refusal("teamConflictCost"),
              "a hard literal repeat next to the default: refused at reset");
        check(resetRefusal(copyOf("teamConflictCost").offset("teamConflictCost", 1.0)).empty() &&
                      resetRefusal(copyOf("teamConflictCost").clip("teamConflictCost", false))
                              .empty() &&
                      resetRefusal(copyOf("teamConflictCost")
                                           .offset("teamConflictCost", 1.0)
                                           .clip("teamConflictCost", false)
                                           .soft("teamConflictCost"))
                              .empty(),
              "team conflict: a threshold offset or clip=false with offset 0 "
              "on hard, and the literal offset on soft, are allowed");
    }
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        LeagueSchedule s = buildRandomInstance(/*matches*/ 30, /*seed*/ 7);
        ScoreCalculator::prepare(s);
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::Editor<LeagueSchedule> editor(s, calculator);
        std::mt19937_64 rng(321);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const int matchCount = s.matchCount();
        const int value_count = s.roundCount();
        int mismatches = 0;
        double drift = 0.0;
        const int steps = 3000;
        for (int step = 0; step < steps; ++step) {
            if (unit(rng) < 0.75) {
                const int match = static_cast<int>(rng() % matchCount);
                const int value = unit(rng) < 0.10 ? -1 : static_cast<int>(rng() % value_count);
                editor.assign(match, value);
            } else {
                const int a = static_cast<int>(rng() % matchCount);
                const int b = static_cast<int>(rng() % matchCount);
                const int va = s.matches[a].round;
                const int vb = s.matches[b].round;
                editor.assign(a, vb);
                editor.assign(b, va);
            }
            LeagueSchedule shadow = s;
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            const Score full = scoreWith(fresh, shadow);
            const Score incremental = editor.score();
            drift = std::max(drift, std::fabs(full.soft - incremental.soft));
            // The engine's ScoreChecks::Full rule (scores_agree): hard exact, soft
            // within 1e-6 (the fractional weights accumulate a few ulps).
            if (!optimaru::scores_agree(incremental, full)) {
                if (++mismatches <= 3) {
                    std::printf("  MISMATCH @step %d: incremental=%s full=%s\n", step,
                                incremental.str().c_str(), full.str().c_str());
                }
            }
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0, std::to_string(steps) +
                                       " random moves with repeated, shifted components on both "
                                       "levels: incremental == fresh");
    }
    {
        LeagueSchedule s = buildRandomInstance(/*matches*/ 20, /*seed*/ 11);
        Solver::Config config;
        config.time_limit = std::chrono::seconds(2);
        config.seed = 5;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruinRecreateUnimprovedStepTrigger = 200;
        config.ruinRecreatePolishSteps = 100;
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        bool corrupted = false;
        try {
            Solver solver(config);
            solver.solve(s, calculator);
        } catch (const optimaru::ScoreMismatchError& e) {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted, "2s FULL_ASSERT solve with repeated, shifted components");
        DecomposedCalculator fresh;
        repeatedAssembly().into(fresh);
        check(!corrupted && optimaru::scores_agree(s.score, scoreWith(fresh, s)),
              "the reported score == a fresh recompute of the same assembly");
    }
    std::printf("\n");
}

}  // namespace

int main() {
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

    if (failures == 0) {
        std::printf("All sports-league-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d sports-league-scheduling plugin check(s) FAILED.\n", failures);
    return 1;
}

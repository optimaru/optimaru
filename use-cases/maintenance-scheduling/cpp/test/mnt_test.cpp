// Maintenance-scheduling unit tests (engine test/ style: check() + non-zero
// exit on any failure):
//   (a) every constraint verified against hand-computed scores — the same
//       cases as the Java MaintenanceSchedulingConstraintProviderTest, with
//       expectations widened to the FULL score (the Java ConstraintVerifier
//       isolates one constraint per assertion; a full-score calculator
//       cannot, so cross-firing constraints are accounted for in comments)
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's ScoreDirector protocol (before/set/after)
//   (b2) the incrementally maintained hot (tag, day) set (the solver's
//       violation-guided entity selection reads it) == a brute-force
//       recomputation, across the same kind of random walk
//   (c) a small instance solved under EnvironmentMode::FULL_ASSERT
//   (d) the SMALL Java demo instance reaches 0hard; pinned jobs keep their
//       assignment through a solve
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "mnt/demo_data.hpp"
#include "mnt/domain.hpp"
#include "mnt/incremental_calculator.hpp"
#include "mnt/score_calculator.hpp"
#include "mnt/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace mnt;

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
    std::printf("  [%s] %-42s got=%s expected=%lldhard/%gsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedSoft);
    if (!ok)
        ++failures;
}

// 2021-02-01 (a Monday) == epoch day 18659, the Java test's DAY_1.
constexpr std::int64_t DAY_1 = 18659;
constexpr std::int64_t DAY_2 = DAY_1 + 1;  // Tuesday
constexpr std::int64_t DAY_3 = DAY_1 + 2;  // Wednesday
constexpr std::int64_t FRIDAY = DAY_1 + 4;
constexpr std::int64_t NEXT_MONDAY = DAY_1 + 7;

struct JobSpec
{
    int durationInDays;
    std::int64_t minStartDate;
    std::int64_t maxEndDate;
    std::int64_t idealEndDate;
    std::vector<std::string> tags;
    int crewIndex;           // -1 == unassigned
    std::int64_t startDate;  // kNoDate == unassigned
};

// Builds a 2-crew (ALPHA=0, BETA=1), 4-week schedule starting DAY_1, adds
// the jobs, prepares, applies the assignments and returns the full score.
Score scoreOf(const std::vector<JobSpec> &specs)
{
    MaintenanceSchedule s;
    s.fromDate = DAY_1;
    s.toDate = DAY_1 + 28;
    s.crews.push_back({"1", "Alpha crew"});
    s.crews.push_back({"2", "Beta crew"});
    for (const JobSpec &spec : specs)
    {
        Job job;
        job.id = "J" + std::to_string(s.jobs.size());
        job.name = job.id;
        job.durationInDays = spec.durationInDays;
        job.minStartDate = spec.minStartDate;
        job.maxEndDate = spec.maxEndDate;
        job.idealEndDate = spec.idealEndDate;
        job.tags = spec.tags;
        s.jobs.push_back(std::move(job));
    }
    ScoreCalculator::prepare(s);
    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        const JobSpec &spec = specs[i];
        if (spec.crewIndex < 0 || spec.startDate == kNoDate)
            continue;
        int dateIndex = -1;
        for (int d = 0; d < s.dateCount(); ++d)
        {
            if (s.startDates[d] == spec.startDate)
            {
                dateIndex = d;
                break;
            }
        }
        check(dateIndex >= 0, "test start date is a business day in range");
        s.jobs[i].crewDate = s.crewDateOf(dateIndex, spec.crewIndex);
    }
    return ScoreCalculator::calculateScore(s);
}

// -------------------------------------------------------------------------
// (a) The Java ConstraintProviderTest cases, full-score expectations.
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Verifying each constraint (Java test cases) ==\n");
    const std::vector<std::string> none;

    // crewConflict: same crew + overlapping days, -overlap hard.
    checkScore("crewConflict same day",
               scoreOf({{1, kNoDate, kNoDate, kNoDate, none, 0, DAY_1},
                        {1, kNoDate, kNoDate, kNoDate, none, 0, DAY_1}}),
               -1,
               0);
    checkScore("crewConflict different day",
               scoreOf({{1, kNoDate, kNoDate, kNoDate, none, 0, DAY_1},
                        {1, kNoDate, kNoDate, kNoDate, none, 0, DAY_2}}),
               0,
               0);
    // [Mon,Thu) vs [Tue,Fri): overlap Tue+Wed = 2.
    checkScore("crewConflict 3d jobs shifted by 1",
               scoreOf({{3, kNoDate, kNoDate, kNoDate, none, 0, DAY_1},
                        {3, kNoDate, kNoDate, kNoDate, none, 0, DAY_2}}),
               -2,
               0);
    checkScore("crewConflict different crews",
               scoreOf({{1, kNoDate, kNoDate, kNoDate, none, 0, DAY_1},
                        {1, kNoDate, kNoDate, kNoDate, none, 1, DAY_1}}),
               0,
               0);
    // Calendar-day overlap includes weekends inside both intervals:
    // dur 6 from Friday covers [Fri, Sat+8) = 8 calendar days.
    checkScore("crewConflict overlap counts weekend days",
               scoreOf({{6, kNoDate, kNoDate, kNoDate, none, 0, FRIDAY},
                        {6, kNoDate, kNoDate, kNoDate, none, 0, FRIDAY}}),
               -8,
               0);

    // minStartDate: -days started early, hard.
    checkScore("minStartDate on time",
               scoreOf({{1, DAY_2, kNoDate, kNoDate, none, 0, DAY_2}}),
               0,
               0);
    checkScore("minStartDate later is fine",
               scoreOf({{1, DAY_1, kNoDate, kNoDate, none, 0, DAY_3}}),
               0,
               0);
    checkScore("minStartDate 2 days early",
               scoreOf({{1, DAY_3, kNoDate, kNoDate, none, 0, DAY_1}}),
               -2,
               0);
    checkScore("minStartDate independent of duration",
               scoreOf({{4, DAY_3, kNoDate, kNoDate, none, 0, DAY_1}}),
               -2,
               0);

    // maxEndDate: -days ended late, hard. end(dur 1 @ Tue) = Wed.
    checkScore("maxEndDate 1 day late",
               scoreOf({{1, kNoDate, DAY_2, kNoDate, none, 0, DAY_2}}),
               -1,
               0);
    // end(dur 1 @ Wed) = Thu; Thu - Mon = 3.
    checkScore("maxEndDate 3 days late",
               scoreOf({{1, kNoDate, DAY_1, kNoDate, none, 0, DAY_3}}),
               -3,
               0);
    checkScore("maxEndDate on time",
               scoreOf({{1, kNoDate, DAY_3, kNoDate, none, 0, DAY_1}}),
               0,
               0);
    // end(dur 4 @ Mon) = Fri; Fri - Wed = 2.
    checkScore("maxEndDate 2 days late (dur 4)",
               scoreOf({{4, kNoDate, DAY_3, kNoDate, none, 0, DAY_1}}),
               -2,
               0);

    // beforeIdealEndDate: -days ended early, soft x1 (duration 0 cases,
    // exactly the Java test; end == start).
    checkScore("beforeIdealEndDate on time",
               scoreOf({{0, kNoDate, kNoDate, DAY_2, none, 0, DAY_2}}),
               0,
               0);
    // end(=DAY_3) is AFTER ideal DAY_1: the afterIdealEndDate constraint
    // cross-fires with 2 days x 1'000'000 (the Java verifier isolates
    // beforeIdealEndDate and sees 0).
    checkScore("beforeIdealEndDate later (after fires)",
               scoreOf({{0, kNoDate, kNoDate, DAY_1, none, 0, DAY_3}}),
               0,
               -2'000'000);
    checkScore("beforeIdealEndDate 2 days early",
               scoreOf({{0, kNoDate, kNoDate, DAY_3, none, 0, DAY_1}}),
               0,
               -2);

    // afterIdealEndDate: -days ended late, soft x1'000'000.
    checkScore("afterIdealEndDate 1 day late",
               scoreOf({{1, kNoDate, kNoDate, DAY_2, none, 0, DAY_2}}),
               0,
               -1'000'000);
    checkScore("afterIdealEndDate 3 days late",
               scoreOf({{1, kNoDate, kNoDate, DAY_1, none, 0, DAY_3}}),
               0,
               -3'000'000);
    // end(dur 1 @ Mon) = Tue, 1 day BEFORE ideal Wed: beforeIdealEndDate
    // cross-fires with -1 (the Java verifier sees after == 0).
    checkScore("afterIdealEndDate earlier (before fires)",
               scoreOf({{1, kNoDate, kNoDate, DAY_3, none, 0, DAY_1}}),
               0,
               -1);
    checkScore("afterIdealEndDate 2 days late (dur 4)",
               scoreOf({{4, kNoDate, kNoDate, DAY_3, none, 0, DAY_1}}),
               0,
               -2'000'000);

    // tagConflict: -1'000 x shared tags x overlapping days, soft. Both jobs
    // are on the SAME crew like in the Java test, so crewConflict adds
    // -overlap hard on the overlapping cases.
    checkScore(
        "tagConflict no overlap",
        scoreOf(
            {{1, kNoDate, kNoDate, kNoDate, {"Downtown"}, 0, DAY_1},
             {1, kNoDate, kNoDate, kNoDate, {"Downtown", "Crane"}, 0, DAY_3}}),
        0,
        0);
    checkScore(
        "tagConflict 1 tag 1 day",
        scoreOf(
            {{1, kNoDate, kNoDate, kNoDate, {"Downtown"}, 0, DAY_1},
             {1, kNoDate, kNoDate, kNoDate, {"Downtown", "Crane"}, 0, DAY_1}}),
        -1,
        -1'000);
    checkScore(
        "tagConflict disjoint tags",
        scoreOf(
            {{1, kNoDate, kNoDate, kNoDate, {"Downtown"}, 0, DAY_1},
             {1, kNoDate, kNoDate, kNoDate, {"Uptown", "Crane"}, 0, DAY_1}}),
        -1,
        0);
    checkScore(
        "tagConflict 2 tags 1 day",
        scoreOf(
            {{1, kNoDate, kNoDate, kNoDate, {"Downtown", "Crane"}, 0, DAY_2},
             {1, kNoDate, kNoDate, kNoDate, {"Downtown", "Crane"}, 0, DAY_2}}),
        -1,
        -2'000);
    // [Mon,Sat) vs [Tue,Fri): overlap 3 days x 2 tags = 6.
    checkScore(
        "tagConflict 2 tags 3 days",
        scoreOf(
            {{5, kNoDate, kNoDate, kNoDate, {"Downtown", "Crane"}, 0, DAY_1},
             {3, kNoDate, kNoDate, kNoDate, {"Downtown", "Crane"}, 0, DAY_2}}),
        -3,
        -6'000);

    // Unassigned jobs contribute nothing.
    checkScore("unassigned contributes nothing",
               scoreOf({{1, DAY_3, kNoDate, kNoDate, none, 0, DAY_1},
                        {9, DAY_3, DAY_1, DAY_1, {"Downtown"}, -1, kNoDate}}),
               -2,
               0);

    // The weekend jump of the endDate shadow: dur 5 @ Mon ends Saturday,
    // dur 6 @ Mon ends the NEXT Tuesday (Sat/Sun skipped). ideal = next
    // Monday -> dur 5 is 2 days early (Sat->Mon), dur 6 is 1 day late.
    checkScore("endDate skips weekend (before)",
               scoreOf({{5, kNoDate, kNoDate, NEXT_MONDAY, none, 0, DAY_1}}),
               0,
               -2);
    checkScore("endDate skips weekend (after)",
               scoreOf({{6, kNoDate, kNoDate, NEXT_MONDAY, none, 0, DAY_1}}),
               0,
               -1'000'000);
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random mid-size instance for the incremental cross-check and the solves.
// -------------------------------------------------------------------------
MaintenanceSchedule buildRandomInstance(int jobCount, std::uint64_t seed)
{
    MaintenanceSchedule s;
    s.fromDate = DAY_1;
    s.toDate = DAY_1 + 8 * 7;  // 8 weeks
    s.crews.push_back({"1", "Alpha crew"});
    s.crews.push_back({"2", "Beta crew"});
    s.crews.push_back({"3", "Gamma crew"});
    const std::vector<std::string> tagPool
        = {"Downtown", "Uptown", "Park", "Airport", "Subway"};
    std::mt19937_64 rng(seed);
    for (int i = 0; i < jobCount; ++i)
    {
        Job job;
        job.id = std::to_string(i);
        job.name = "Job " + std::to_string(i);
        job.durationInDays = static_cast<int>(rng() % 11);  // 0..10
        const std::int64_t anchor
            = s.fromDate + static_cast<std::int64_t>(rng() % 40);
        if (rng() % 4 != 0)
            job.minStartDate = anchor;
        if (rng() % 4 != 0)
            job.maxEndDate = anchor + 7 + static_cast<std::int64_t>(rng() % 14);
        if (rng() % 4 != 0)
            job.idealEndDate
                = anchor + 4 + static_cast<std::int64_t>(rng() % 9);
        const int tagCount = static_cast<int>(rng() % 3);  // 0..2 tags
        for (int t = 0; t < tagCount; ++t)
            job.tags.push_back(tagPool[rng() % tagPool.size()]);
        s.jobs.push_back(std::move(job));
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
    MaintenanceSchedule s = buildRandomInstance(/*jobCount*/ 24, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    MaintenanceSchedulingIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<MaintenanceSchedule> director(s, calc);
    MaintenanceSchedule shadow = s;

    std::mt19937_64 rng(123);
    const int jobCount = static_cast<int>(s.jobs.size());
    const int valueCount = s.crewDateCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            // ChangeMove; 10% of the time to unassigned (-1), like the
            // construction heuristic's starting state.
            const int job = static_cast<int>(rng() % jobCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(job, value);
        }
        else
        {
            // SwapMove.
            const int a = static_cast<int>(rng() % jobCount);
            const int b = static_cast<int>(rng() % jobCount);
            const int va = s.jobs[a].crewDate;
            const int vb = s.jobs[b].crewDate;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < jobCount; ++i)
            shadow.jobs[i].crewDate = s.jobs[i].crewDate;
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
// (b2) The hot (tag, day) set the solver's entityRecommender samples from is
// maintained incrementally inside insert/retract, so — like the score itself
// — it has to survive a random walk. It must be exactly the set of cells
// occupied by two or more jobs (the cells that carry a tagConflict).
// -------------------------------------------------------------------------
void verifyHotTagCells()
{
    std::printf("== (b2) Verifying the incremental hot (tag, day) set ==\n");
    MaintenanceSchedule s = buildRandomInstance(/*jobCount*/ 24, /*seed*/ 11);
    ScoreCalculator::prepare(s);

    MaintenanceSchedulingIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<MaintenanceSchedule> director(s, calc);

    std::mt19937_64 rng(4242);
    const int jobCount = static_cast<int>(s.jobs.size());
    const int valueCount = s.crewDateCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    int sawHot = 0;
    const int steps = 2000;
    for (int step = 0; step < steps; ++step)
    {
        const int job = static_cast<int>(rng() % jobCount);
        director.changeVariable(
            job, unit(rng) < 0.10 ? -1 : static_cast<int>(rng() % valueCount));
        director.calculateScore();

        // Brute-force reference: count the occupants of every (tag, day) cell.
        std::vector<int> counts(
            static_cast<std::size_t>(s.tagCount) * s.dayCount, 0);
        for (const Job &j : s.jobs)
        {
            if (j.crewDate < 0)
                continue;
            const int dateIndex = s.dateIndexOf(j.crewDate);
            const std::int64_t end = s.endDateFor(j.durationInDays, dateIndex);
            for (std::int64_t day = s.startDates[dateIndex]; day < end; ++day)
            {
                const int offset = static_cast<int>(day - s.dayBase);
                for (int tag : j.tagIds)
                    ++counts[static_cast<std::size_t>(tag) * s.dayCount
                             + offset];
            }
        }
        std::vector<int> expected;
        for (std::size_t cell = 0; cell < counts.size(); ++cell)
        {
            if (counts[cell] >= 2)
                expected.push_back(static_cast<int>(cell));
        }
        std::vector<int> actual = calc.hotTagCells();
        std::sort(actual.begin(), actual.end());
        if (actual != expected)
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @step %d: %zu hot cells, expected "
                            "%zu\n",
                            step,
                            actual.size(),
                            expected.size());
            }
        }
        if (!expected.empty())
            ++sawHot;
    }
    check(mismatches == 0,
          std::to_string(steps) + " random moves, hot set == brute force");
    check(sawHot > steps / 10,
          "the walk actually produced tag conflicts (" + std::to_string(sawHot)
              + " steps)");
    check(calc.hasHotTagCells() == !calc.hotTagCells().empty(),
          "hasHotTagCells agrees with the set");
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
    MaintenanceSchedule s = buildRandomInstance(/*jobCount*/ 16, /*seed*/ 11);

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
// (d) The Java SMALL demo instance (fixed Monday for reproducibility)
// reaches 0hard, and a pinned job keeps its assignment through a solve.
// -------------------------------------------------------------------------
void verifyDemoAndPinnedSolve()
{
    std::printf("== (d) SMALL demo reaches 0hard; pinning holds ==\n");
    MaintenanceSchedule s = generateDemoData(DemoData::SMALL, DAY_1);
    check(static_cast<int>(s.jobs.size()) == 14 && s.crewCount() == 3,
          "SMALL demo: 3 crews, 14 jobs (Java jobListSize)");
    ScoreCalculator::prepare(s);

    // Pin job 0 to its earliest in-window start on Gamma crew.
    int pinnedValue = -1;
    for (int d = 0; d < s.dateCount(); ++d)
    {
        const Job &job = s.jobs[0];
        if (s.startDates[d] >= job.minStartDate
            && s.endDateFor(job.durationInDays, d) <= job.maxEndDate)
        {
            pinnedValue = s.crewDateOf(d, 2);
            break;
        }
    }
    check(pinnedValue >= 0, "found a feasible pin for job 0");
    s.jobs[0].crewDate = pinnedValue;
    s.jobs[0].pinned = true;

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(8);
    config.bestScoreLimit = Score{0, -1e18};  // stop at first feasible
    config.randomSeed = 5;
    Solver solver(config);
    solver.solve(s);

    check(s.jobs[0].crewDate == pinnedValue, "pinned job kept its crewDate");
    bool allAssigned = true;
    for (const Job &job : s.jobs)
    {
        if (job.crewDate < 0)
            allAssigned = false;
    }
    check(allAssigned, "every job assigned after the solve");
    check(s.score.hard == 0, "SMALL demo reaches 0hard");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyIncremental();
    verifyHotTagCells();
    verifyFullAssertSolve();
    verifyDemoAndPinnedSolve();

    if (failures == 0)
    {
        std::printf("All maintenance-scheduling checks PASSED.\n");
        return 0;
    }
    std::printf("%d maintenance-scheduling check(s) FAILED.\n", failures);
    return 1;
}

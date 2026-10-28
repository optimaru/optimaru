// Plugin-assembly (mnt/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 400 random
//       assignments (unassigned jobs included)
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

#include "mnt/constraints.hpp"
#include "mnt/domain.hpp"
#include "mnt/score_calculator.hpp"
#include "mnt/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace mnt;

namespace
{

int failures = 0;

// 2021-02-01 (a Monday) == epoch day 18659, the Java test's DAY_1.
constexpr std::int64_t DAY_1 = 18659;

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

// Two crews over an 8-week calendar, no jobs yet.
MaintenanceSchedule makeBase()
{
    MaintenanceSchedule s;
    s.fromDate = DAY_1;
    s.toDate = DAY_1 + 8 * 7;
    s.crews.push_back({"1", "Alpha crew"});
    s.crews.push_back({"2", "Beta crew"});
    return s;
}

int addJob(MaintenanceSchedule &s,
           int durationInDays,
           std::vector<std::string> tags = {},
           std::int64_t minStartDate = kNoDate,
           std::int64_t maxEndDate = kNoDate,
           std::int64_t idealEndDate = kNoDate)
{
    Job job;
    job.id = std::to_string(s.jobs.size());
    job.name = "Job " + job.id;
    job.durationInDays = durationInDays;
    job.minStartDate = minStartDate;
    job.maxEndDate = maxEndDate;
    job.idealEndDate = idealEndDate;
    job.tags = std::move(tags);
    s.jobs.push_back(std::move(job));
    return static_cast<int>(s.jobs.size()) - 1;
}

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

Score scoreWith(DecomposedCalculator &calculator, MaintenanceSchedule &s)
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
    MaintenanceSchedule s = buildRandomInstance(/*jobCount*/ 24, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int mismatches = 0;
    const int rounds = 400;
    for (int round = 0; round < rounds; ++round)
    {
        for (Job &job : s.jobs)
        {
            job.crewDate = unit(rng) < 0.10
                               ? -1
                               : static_cast<int>(rng() % s.crewDateCount());
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
    MaintenanceSchedule s = buildRandomInstance(/*jobCount*/ 24, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<MaintenanceSchedule> director(s, calculator);
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
            const int job = static_cast<int>(rng() % jobCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(job, value);
        }
        else
        {
            const int a = static_cast<int>(rng() % jobCount);
            const int b = static_cast<int>(rng() % jobCount);
            const int va = s.jobs[a].crewDate;
            const int vb = s.jobs[b].crewDate;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        for (int i = 0; i < jobCount; ++i)
            shadow.jobs[i].crewDate = s.jobs[i].crewDate;
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

    {  // crewConflict alone: two 5-business-day jobs on the same crew and
        // start date overlap on all 7 calendar days they span.
        MaintenanceSchedule s = makeBase();
        addJob(s, 5);
        addJob(s, 5);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        s.jobs[1].crewDate = s.crewDateOf(0, 0);
        const std::int64_t days
            = s.endDateFor(5, 0) - s.startDates[0];  // 5 business days -> 7
        DecomposedCalculator calculator;
        calculator.emplace<CrewConflict>();
        checkScore("crewConflict alone", scoreWith(calculator, s), -days, 0.0);
    }
    {  // Same two jobs on DIFFERENT crews: no conflict at all.
        MaintenanceSchedule s = makeBase();
        addJob(s, 5);
        addJob(s, 5);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        s.jobs[1].crewDate = s.crewDateOf(0, 1);
        DecomposedCalculator calculator;
        calculator.emplace<CrewConflict>();
        checkScore("crewConflict silent across crews",
                   scoreWith(calculator, s),
                   0,
                   0.0);
    }
    {  // tagConflict alone: the same two jobs on different crews, sharing one
        // tag, at 1'000 per overlapping day.
        MaintenanceSchedule s = makeBase();
        addJob(s, 5, {"Downtown"});
        addJob(s, 5, {"Downtown"});
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        s.jobs[1].crewDate = s.crewDateOf(0, 1);
        const std::int64_t days = s.endDateFor(5, 0) - s.startDates[0];
        DecomposedCalculator calculator;
        calculator.emplace<TagConflict>();
        checkScore("tagConflict alone",
                   scoreWith(calculator, s),
                   0,
                   -1000.0 * static_cast<double>(days));
    }
    {  // minStartDate alone: starting 3 days early costs 3.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, /*minStartDate*/ DAY_1 + 3);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);  // starts on DAY_1
        DecomposedCalculator calculator;
        calculator.emplace<MinStartDate>();
        checkScore("minStartDate alone", scoreWith(calculator, s), -3, 0.0);
    }
    {  // maxEndDate alone: a 2-business-day job from DAY_1 ends on DAY_1 + 2,
        // one day past a DAY_1 + 1 deadline.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, kNoDate, /*maxEndDate*/ DAY_1 + 1);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        DecomposedCalculator calculator;
        calculator.emplace<MaxEndDate>();
        checkScore("maxEndDate alone", scoreWith(calculator, s), -1, 0.0);
    }
    {  // beforeIdealEndDate alone: ending on DAY_1 + 2 with an ideal of
        // DAY_1 + 5 is 3 days early, at weight 1.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, kNoDate, kNoDate, /*idealEndDate*/ DAY_1 + 5);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        DecomposedCalculator calculator;
        calculator.emplace<BeforeIdealEndDate>();
        checkScore(
            "beforeIdealEndDate alone", scoreWith(calculator, s), 0, -3.0);
    }
    {  // afterIdealEndDate alone: the same job one day late costs 1'000'000.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, kNoDate, kNoDate, /*idealEndDate*/ DAY_1 + 1);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        DecomposedCalculator calculator;
        calculator.emplace<AfterIdealEndDate>();
        checkScore(
            "afterIdealEndDate alone", scoreWith(calculator, s), 0, -1000000.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    MaintenanceSchedule s = makeBase();
    addJob(s, 2, {}, /*minStartDate*/ DAY_1 + 3);
    ScoreCalculator::prepare(s);
    s.jobs[0].crewDate = s.crewDateOf(0, 0);

    {
        DecomposedCalculator calculator;
        calculator.emplace<MinStartDate>(10.0);
        checkScore("minStartDate re-weighted to 10",
                   scoreWith(calculator, s),
                   -30,
                   0.0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<MinStartDate>();
        component->isHard = false;
        calculator.add(std::move(component));
        checkScore(
            "minStartDate demoted to soft", scoreWith(calculator, s), 0, -3.0);
    }
    {
        // exponent 2 shapes the raw day count (3) before the weight.
        DecomposedCalculator calculator;
        auto component = std::make_unique<MinStartDate>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore(
            "minStartDate with exponent 2", scoreWith(calculator, s), -9, 0.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    MaintenanceSchedule s = buildRandomInstance(/*jobCount*/ 16, /*seed*/ 13);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(5);
    for (Job &job : s.jobs)
        job.crewDate = static_cast<int>(rng() % s.crewDateCount());

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
    check(calculator.constraintTotals().size() == 6,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    MaintenanceSchedule s = buildRandomInstance(/*jobCount*/ 16, /*seed*/ 11);

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
        std::printf("All maintenance-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d maintenance-scheduling plugin check(s) FAILED.\n",
                failures);
    return 1;
}

// Plugin-assembly (pjs/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 300 random
//       assignments (unassigned allocations included)
//   (b) incremental score == full recompute across 1500 random moves driven
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

#include "pjs/constraints.hpp"
#include "pjs/domain.hpp"
#include "pjs/score_calculator.hpp"
#include "pjs/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace pjs;

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
                long expectedMedium,
                long expectedSoft)
{
    const bool ok = actual.hard == expectedHard
                    && actual.medium == expectedMedium
                    && actual.soft == expectedSoft;
    std::printf("  [%s] %-44s got=%s expected=%ldhard/%ldmedium/%ldsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedMedium,
                expectedSoft);
    if (!ok)
        ++failures;
}

// One project, one resource; jobs/allocations appended by addJob below.
ProjectJobSchedule
makeBase(int resourceCapacity, bool renewable, int criticalPathDuration)
{
    ProjectJobSchedule schedule;
    schedule.projects.push_back({"1", 0, criticalPathDuration});
    schedule.resources.push_back({"1", resourceCapacity, renewable, 0});
    return schedule;
}

int addJob(ProjectJobSchedule &schedule,
           JobType jobType,
           int duration,
           int requirement,  // <= 0: no resource requirement
           const std::vector<int> &predecessors,
           int delay = -1)  // < 0: unassigned
{
    Job job;
    job.id = std::to_string(schedule.jobs.size());
    job.project = 0;
    job.jobType = jobType;
    ExecutionMode mode;
    mode.id = "m" + job.id;
    mode.duration = duration;
    if (requirement > 0)
        mode.resourceRequirements.push_back({0, requirement});
    job.executionModes.push_back(mode);
    schedule.jobs.push_back(job);

    Allocation allocation;
    allocation.id = job.id;
    allocation.job = static_cast<int>(schedule.jobs.size()) - 1;
    allocation.predecessorAllocations = predecessors;
    schedule.allocations.push_back(allocation);
    const int index = schedule.allocationCount() - 1;
    if (delay >= 0)
    {
        // Single-mode jobs everywhere in these fixtures: maxModeCount == 1,
        // so the composite value is simply the delay.
        schedule.allocations[index].modeDelay = delay;
    }
    return index;
}

ProjectJobSchedule buildRandomInstance(int projectCount,
                                       int standardPerProject,
                                       std::uint64_t seed,
                                       int delayRangeSize)
{
    std::mt19937_64 rng(seed);
    ProjectJobSchedule schedule;
    schedule.delayRangeSize = delayRangeSize;

    schedule.resources.push_back({"global", 12, true, -1});
    for (int p = 0; p < projectCount; ++p)
    {
        schedule.projects.push_back(
            {"P" + std::to_string(p), 2 * p, 10 + 3 * p});
        schedule.resources.push_back({"ren" + std::to_string(p), 10, true, p});
        schedule.resources.push_back({"non" + std::to_string(p), 30, false, p});
    }

    for (int p = 0; p < projectCount; ++p)
    {
        const int base = static_cast<int>(schedule.jobs.size());
        const int count = standardPerProject + 2;
        const std::vector<int> pool = {0, 1 + 2 * p, 2 + 2 * p};
        for (int i = 0; i < count; ++i)
        {
            Job job;
            job.id = "J" + std::to_string(base + i);
            job.project = p;
            job.jobType = i == 0           ? JobType::SOURCE
                          : i == count - 1 ? JobType::SINK
                                           : JobType::STANDARD;
            if (job.jobType == JobType::STANDARD)
            {
                const int modeCount = 1 + static_cast<int>(rng() % 3);
                for (int m = 0; m < modeCount; ++m)
                {
                    ExecutionMode mode;
                    mode.id = job.id + "m" + std::to_string(m);
                    mode.duration = 1 + static_cast<int>(rng() % 5);
                    const int reqCount = 1 + static_cast<int>(rng() % 2);
                    for (int r = 0; r < reqCount; ++r)
                    {
                        mode.resourceRequirements.push_back(
                            {pool[rng() % pool.size()],
                             1 + static_cast<int>(rng() % 5)});
                    }
                    job.executionModes.push_back(mode);
                }
            }
            else
            {
                job.executionModes.push_back({job.id + "m0", 0, {}});
            }
            schedule.jobs.push_back(job);

            Allocation allocation;
            allocation.id = job.id;
            allocation.job = base + i;
            if (job.jobType != JobType::STANDARD)
                allocation.modeDelay = 0;  // preassigned (mode 0, delay 0)
            schedule.allocations.push_back(allocation);
        }
        for (int i = 1; i < count - 1; ++i)
        {
            schedule.allocations[base + i].predecessorAllocations.push_back(
                i <= 2 ? base : base + i - 1);
            if (i >= 3 && rng() % 2 == 0)
            {
                schedule.allocations[base + i].predecessorAllocations.push_back(
                    base + 1 + static_cast<int>(rng() % (i - 1)));
            }
        }
        schedule.allocations[base + count - 1].predecessorAllocations.push_back(
            base + count - 2);
    }
    return schedule;
}

Score scoreWith(DecomposedCalculator &calculator, ProjectJobSchedule &s)
{
    ScoreCalculator::prepare(s);  // idempotent; the components require it
    calculator.resetWorkingSolution(s);
    return fromEngineScore(calculator.calculateScore());
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    ProjectJobSchedule s = buildRandomInstance(/*projects*/ 2,
                                               /*standardPerProject*/ 6,
                                               /*seed*/ 7,
                                               /*delayRangeSize*/ 12);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int mismatches = 0;
    const int rounds = 300;
    for (int round = 0; round < rounds; ++round)
    {
        for (int a = 0; a < s.allocationCount(); ++a)
        {
            if (s.jobTypeOf(a) != JobType::STANDARD)
                continue;  // sources/sinks stay preassigned
            s.allocations[a].modeDelay
                = unit(rng) < 0.10 ? -1
                                   : static_cast<int>(rng() % s.valueCount());
        }
        const Score plugin = scoreWith(calculator, s);
        const Score monolithic = ScoreCalculator::calculateScore(s);
        if (!(plugin == monolithic))
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
    ProjectJobSchedule s = buildRandomInstance(/*projects*/ 2,
                                               /*standardPerProject*/ 6,
                                               /*seed*/ 7,
                                               /*delayRangeSize*/ 12);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<ProjectJobSchedule> director(s, calculator);
    ProjectJobSchedule shadow = s;

    std::mt19937_64 rng(123);
    std::vector<int> movable;
    for (int a = 0; a < s.allocationCount(); ++a)
    {
        if (s.jobTypeOf(a) == JobType::STANDARD)
            movable.push_back(a);
    }
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 1500;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            const int a = movable[rng() % movable.size()];
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % s.valueCount());
            director.changeVariable(a, value);
        }
        else
        {
            const int a = movable[rng() % movable.size()];
            const int b = movable[rng() % movable.size()];
            const int va = s.allocations[a].modeDelay;
            const int vb = s.allocations[b].modeDelay;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        for (int i = 0; i < s.allocationCount(); ++i)
            shadow.allocations[i].modeDelay = s.allocations[i].modeDelay;
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = fromEngineScore(director.calculateScore());
        if (!(full == incremental))
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
// (c) Single-component assemblies on the Java provider test's shapes.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Single-component assemblies ==\n");

    {  // nonRenewableResourceCapacity alone: 7 + 6 against a capacity of 10.
        ProjectJobSchedule s = makeBase(10, /*renewable*/ false, 0);
        addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        addJob(s, JobType::STANDARD, 1, 7, {0}, 0);
        addJob(s, JobType::STANDARD, 1, 6, {0}, 0);
        DecomposedCalculator calculator;
        calculator.emplace<NonRenewableResourceCapacity>();
        checkScore("nonRenewableResourceCapacity alone",
                   scoreWith(calculator, s),
                   -3,
                   0,
                   0);
    }
    {  // Within capacity: silent.
        ProjectJobSchedule s = makeBase(13, /*renewable*/ false, 0);
        addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        addJob(s, JobType::STANDARD, 1, 7, {0}, 0);
        addJob(s, JobType::STANDARD, 1, 6, {0}, 0);
        DecomposedCalculator calculator;
        calculator.emplace<NonRenewableResourceCapacity>();
        checkScore("nonRenewableResourceCapacity within capacity",
                   scoreWith(calculator, s),
                   0,
                   0,
                   0);
    }
    {  // renewableResourceCapacity alone: two overlapping jobs needing 7 + 6
        // on one day against a capacity of 10.
        ProjectJobSchedule s = makeBase(10, /*renewable*/ true, 0);
        addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        addJob(s, JobType::STANDARD, 1, 7, {0}, 0);
        addJob(s, JobType::STANDARD, 1, 6, {0}, 0);
        DecomposedCalculator calculator;
        calculator.emplace<RenewableResourceCapacity>();
        checkScore("renewableResourceCapacity alone",
                   scoreWith(calculator, s),
                   -3,
                   0,
                   0);
    }
    {  // The same two jobs on DIFFERENT days: a renewable resource refreshes,
        // so nothing is over capacity.
        ProjectJobSchedule s = makeBase(10, /*renewable*/ true, 0);
        addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        addJob(s, JobType::STANDARD, 1, 7, {0}, /*delay*/ 0);
        addJob(s, JobType::STANDARD, 1, 6, {0}, /*delay*/ 1);
        DecomposedCalculator calculator;
        calculator.emplace<RenewableResourceCapacity>();
        checkScore("renewableResourceCapacity on separate days",
                   scoreWith(calculator, s),
                   0,
                   0,
                   0);
    }
    {  // totalProjectDelay alone: a sink ending on day 3 against a critical
        // path end date of 1.
        ProjectJobSchedule s = makeBase(100, true, /*criticalPathDuration*/ 1);
        addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        addJob(s, JobType::STANDARD, 3, 0, {0}, 0);
        addJob(s, JobType::SINK, 0, 0, {1}, 0);
        DecomposedCalculator calculator;
        calculator.emplace<TotalProjectDelay>();
        checkScore(
            "totalProjectDelay alone", scoreWith(calculator, s), 0, -2, 0);
    }
    {  // totalMakespan alone: the same schedule's last sink end date.
        ProjectJobSchedule s = makeBase(100, true, /*criticalPathDuration*/ 1);
        addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        addJob(s, JobType::STANDARD, 3, 0, {0}, 0);
        addJob(s, JobType::SINK, 0, 0, {1}, 0);
        DecomposedCalculator calculator;
        calculator.emplace<TotalMakespan>();
        checkScore("totalMakespan alone", scoreWith(calculator, s), 0, 0, -3);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    ProjectJobSchedule s = makeBase(10, /*renewable*/ false, 0);
    addJob(s, JobType::SOURCE, 0, 0, {}, 0);
    addJob(s, JobType::STANDARD, 1, 7, {0}, 0);
    addJob(s, JobType::STANDARD, 1, 6, {0}, 0);

    {
        DecomposedCalculator calculator;
        calculator.emplace<NonRenewableResourceCapacity>(10.0);
        checkScore("nonRenewableResourceCapacity re-weighted to 10",
                   scoreWith(calculator, s),
                   -30,
                   0,
                   0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<NonRenewableResourceCapacity>();
        component->isHard = false;
        calculator.add(std::move(component));
        checkScore("nonRenewableResourceCapacity demoted to soft",
                   scoreWith(calculator, s),
                   0,
                   0,
                   -3);
    }
    {
        // exponent 2 shapes the raw excess (3) before the weight.
        DecomposedCalculator calculator;
        auto component = std::make_unique<NonRenewableResourceCapacity>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("nonRenewableResourceCapacity with exponent 2",
                   scoreWith(calculator, s),
                   -9,
                   0,
                   0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    ProjectJobSchedule s = buildRandomInstance(/*projects*/ 2,
                                               /*standardPerProject*/ 5,
                                               /*seed*/ 13,
                                               /*delayRangeSize*/ 10);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(5);
    for (int a = 0; a < s.allocationCount(); ++a)
    {
        if (s.jobTypeOf(a) == JobType::STANDARD)
            s.allocations[a].modeDelay
                = static_cast<int>(rng() % s.valueCount());
    }

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(s);
    const optimaru::HardSoftScore score = calculator.calculateScore();

    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-3,
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
    ProjectJobSchedule s = buildRandomInstance(/*projects*/ 2,
                                               /*standardPerProject*/ 4,
                                               /*seed*/ 11,
                                               /*delayRangeSize*/ 8);

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
        check(s.score == recomputed,
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
        std::printf("All project-job-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d project-job-scheduling plugin check(s) FAILED.\n",
                failures);
    return 1;
}

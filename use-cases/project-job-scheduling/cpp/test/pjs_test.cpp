// Project-job-scheduling unit tests (engine test/ style: check() + non-zero
// exit on any failure):
//   (a) every constraint verified against the hand-computed scores of the
//       Java ProjectJobSchedulingConstraintProviderTest cases (the shadow
//       chain included), plus composite-encoding and pass-through checks
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's ScoreDirector protocol (before/set/after)
//   (c) a small instance solved under EnvironmentMode::FULL_ASSERT
//   (d) a demo-sized solve reaches feasibility and keeps pinned allocations
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "pjs/domain.hpp"
#include "pjs/incremental_calculator.hpp"
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

// One project, one resource; jobs/allocations appended by the helpers below.
ProjectJobSchedule
makeBase(int resourceCapacity, bool renewable, int criticalPathDuration)
{
    ProjectJobSchedule schedule;
    schedule.projects.push_back({"1", 0, criticalPathDuration});
    schedule.resources.push_back({"1", resourceCapacity, renewable, 0});
    return schedule;
}

// Adds a job with a single execution mode (duration, one optional
// requirement on resource 0) and its 1:1 allocation. Returns the allocation
// index.
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

// -------------------------------------------------------------------------
// (a) The Java ProjectJobSchedulingConstraintProviderTest cases, hand
// checked against the full recompute (which also exercises the ported
// shadow chain: the Java tests preset predecessorsDoneDate = 1 / 2, here a
// duration-1 / duration-2 predecessor produces the same dates).
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Verifying each constraint in isolation ==\n");

    // hard 1: nonRenewableResourceCapacity — requirements 7 + 6 against
    // capacity 10 (dates are irrelevant): penalizes 13 - 10 = 3.
    {
        ProjectJobSchedule s = makeBase(10, /*renewable*/ false, 0);
        addJob(s, JobType::STANDARD, 10, 7, {}, 100);
        addJob(s, JobType::STANDARD, 10, 6, {}, 100);
        checkScore("nonRenewableResourceCapacity",
                   ScoreCalculator::calculateScore(s),
                   -3,
                   0,
                   0);
    }
    // hard 1: within capacity costs nothing.
    {
        ProjectJobSchedule s = makeBase(13, /*renewable*/ false, 0);
        addJob(s, JobType::STANDARD, 10, 7, {}, 100);
        addJob(s, JobType::STANDARD, 10, 6, {}, 100);
        checkScore("nonRenewable within capacity",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0,
                   0);
    }
    // hard 2: renewableResourceCapacity — requirements 7 (days 101..110)
    // and 5 (days 102..111) against capacity 10: 9 overlap days x 2 = 18.
    // predecessorsDoneDate 1 / 2 comes from duration-1 / duration-2
    // predecessors (the Java test presets the shadow directly).
    {
        ProjectJobSchedule s = makeBase(10, /*renewable*/ true, 0);
        const int p1 = addJob(s, JobType::STANDARD, 1, 0, {}, 0);
        const int p2 = addJob(s, JobType::STANDARD, 2, 0, {}, 0);
        const int a1 = addJob(s, JobType::STANDARD, 10, 7, {p1}, 100);
        const int a2 = addJob(s, JobType::STANDARD, 10, 5, {p2}, 100);
        checkScore("renewableResourceCapacity",
                   ScoreCalculator::calculateScore(s),
                   -18,
                   0,
                   0);
        // The ported shadow chain (predecessorsDoneDate -> start -> end).
        check(s.allocations[a1].predecessorsDoneDate == 1
                  && s.allocations[a1].startDate == 101
                  && s.allocations[a1].endDate == 111,
              "shadow chain of allocation 1 (done=1, start=101, end=111)");
        check(s.allocations[a2].predecessorsDoneDate == 2
                  && s.allocations[a2].startDate == 102
                  && s.allocations[a2].endDate == 112,
              "shadow chain of allocation 2 (done=2, start=102, end=112)");
    }
    // medium 3 + soft 4 (+ hard 2): the Java totalProjectDelay case — SINK
    // ends 111 and 112 against criticalPathEndDate 100 -> medium 11 + 12 =
    // 23; makespan max(111, 112) = 112; the renewable overlap is 18 again.
    {
        ProjectJobSchedule s = makeBase(10, /*renewable*/ true, 100);
        const int p1 = addJob(s, JobType::STANDARD, 1, 0, {}, 0);
        const int p2 = addJob(s, JobType::STANDARD, 2, 0, {}, 0);
        addJob(s, JobType::SINK, 10, 7, {p1}, 100);
        addJob(s, JobType::SINK, 10, 5, {p2}, 100);
        checkScore("totalProjectDelay + totalMakespan",
                   ScoreCalculator::calculateScore(s),
                   -18,
                   -23,
                   -112);
    }
    // medium 3: no penalty when the sinks end before the critical path end
    // (the Java totalMakespan case: criticalPathDuration 155).
    {
        ProjectJobSchedule s = makeBase(10, /*renewable*/ true, 155);
        const int p1 = addJob(s, JobType::STANDARD, 1, 0, {}, 0);
        const int p2 = addJob(s, JobType::STANDARD, 2, 0, {}, 0);
        addJob(s, JobType::SINK, 10, 7, {p1}, 100);
        addJob(s, JobType::SINK, 10, 5, {p2}, 100);
        checkScore("totalMakespan (delay not positive)",
                   ScoreCalculator::calculateScore(s),
                   -18,
                   0,
                   -112);
    }
    // Composite encoding: a 2-mode job inside a 3-mode schedule decodes the
    // slot modulo its own mode count (slot 2 -> mode 0).
    {
        ProjectJobSchedule s = makeBase(100, true, 0);
        // 3-mode job fixes maxModeCount = 3.
        Job wide;
        wide.id = "wide";
        wide.project = 0;
        wide.jobType = JobType::STANDARD;
        wide.executionModes = {{"w0", 1, {}}, {"w1", 1, {}}, {"w2", 1, {}}};
        s.jobs.push_back(wide);
        s.allocations.push_back({"wide", 0, {}, -1, false, 0, 0, 0});
        s.allocations.back().job = static_cast<int>(s.jobs.size()) - 1;
        // 2-mode job: durations 3 and 7.
        Job narrow;
        narrow.id = "narrow";
        narrow.project = 0;
        narrow.jobType = JobType::STANDARD;
        narrow.executionModes = {{"n0", 3, {}}, {"n1", 7, {}}};
        s.jobs.push_back(narrow);
        s.allocations.push_back({"narrow", 0, {}, -1, false, 0, 0, 0});
        s.allocations.back().job = static_cast<int>(s.jobs.size()) - 1;
        const int narrowAllocation = s.allocationCount() - 1;

        ScoreCalculator::prepare(s);
        check(s.maxModeCount == 3, "maxModeCount over all jobs");
        // delay 4, slot 2: value = 4 * 3 + 2 = 14 -> mode 2 % 2 = 0 (dur 3).
        s.allocations[narrowAllocation].modeDelay = s.valueOf(4, 2);
        ScoreCalculator::calculateScore(s);
        check(s.allocations[narrowAllocation].startDate == 4
                  && s.allocations[narrowAllocation].endDate == 7,
              "slot 2 of a 2-mode job decodes to mode 0 (start 4, end 7)");
        // delay 4, slot 1 -> mode 1 (dur 7).
        s.allocations[narrowAllocation].modeDelay = s.valueOf(4, 1);
        ScoreCalculator::calculateScore(s);
        check(s.allocations[narrowAllocation].endDate == 11,
              "slot 1 of a 2-mode job decodes to mode 1 (end 11)");
    }
    // Unassigned allocations pass dates through (delay 0 / duration 0) and
    // contribute no resource usage — the Java suppliers' null-handling.
    {
        ProjectJobSchedule s = makeBase(1, /*renewable*/ true, 0);
        const int a = addJob(s, JobType::STANDARD, 2, 1, {}, 1);
        const int b = addJob(s, JobType::STANDARD, 5, 9, {a});  // unassigned
        const int c = addJob(s, JobType::STANDARD, 3, 1, {b}, 0);
        checkScore("unassigned contributes no usage",
                   ScoreCalculator::calculateScore(s),
                   0,
                   0,
                   0);
        check(s.allocations[b].startDate == 3 && s.allocations[b].endDate == 3
                  && s.allocations[c].startDate == 3
                  && s.allocations[c].endDate == 6,
              "unassigned passes dates through (b 3-3, c 3-6)");
    }
    // Pinning: SOURCE/SINK are always pinned, STANDARD only when flagged.
    {
        ProjectJobSchedule s = makeBase(10, true, 0);
        const int source = addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        const int standard = addJob(s, JobType::STANDARD, 1, 1, {source}, 0);
        const int sink = addJob(s, JobType::SINK, 0, 0, {standard}, 0);
        using Descriptor
            = optimaru::SolutionDescriptor<pjs::ProjectJobSchedule>;
        check(Descriptor::isPinned(s, source) && Descriptor::isPinned(s, sink)
                  && !Descriptor::isPinned(s, standard),
              "SOURCE/SINK pinned, STANDARD not");
        s.allocations[standard].pinned = true;
        check(Descriptor::isPinned(s, standard), "user-pinned STANDARD");
    }
    // prepare() rejects a precedence cycle.
    {
        ProjectJobSchedule s = makeBase(10, true, 0);
        const int a = addJob(s, JobType::STANDARD, 1, 0, {}, 0);
        const int b = addJob(s, JobType::STANDARD, 1, 0, {a}, 0);
        s.allocations[a].predecessorAllocations.push_back(b);
        bool threw = false;
        try
        {
            ScoreCalculator::prepare(s);
        }
        catch (const std::invalid_argument &)
        {
            threw = true;
        }
        check(threw, "prepare() throws on a precedence cycle");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random mid-size instance for the incremental cross-check and the solves:
// `projectCount` projects x (source + `standardPerProject` + sink), jobs
// with 1..3 execution modes (so the composite encoding's modulo decode is
// exercised), a random precedence DAG and a mix of renewable/non-renewable
// resources.
// -------------------------------------------------------------------------
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
        // Precedence DAG (local indices, edges only forward): the source
        // feeds the first standards, every standard feeds 1-2 later jobs,
        // the last standard feeds the sink.
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

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves (including unassignments), driven through the ScoreDirector so
// the before/set/after hook contract is exercised exactly like in the
// engine. Every move can shift whole successor subgraphs, so this covers the
// shadow-chain re-propagation.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Verifying incremental scoring == full recompute ==\n");
    ProjectJobSchedule t = buildRandomInstance(/*projects*/ 2,
                                               /*standardPerProject*/ 6,
                                               /*seed*/ 7,
                                               /*delayRangeSize*/ 40);
    ScoreCalculator::prepare(t);

    ProjectJobSchedulingIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<ProjectJobSchedule> director(t, calc);
    ProjectJobSchedule shadow = t;

    std::mt19937_64 rng(123);
    const int allocationCount = t.allocationCount();
    const int valueCount = t.valueCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            // ChangeMove; 10% of the time to unassigned (-1), like the ruin
            // phase and the construction heuristic's starting state.
            const int allocation = static_cast<int>(rng() % allocationCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(allocation, value);
        }
        else
        {
            // SwapMove (also across jobs with different mode counts).
            const int a = static_cast<int>(rng() % allocationCount);
            const int b = static_cast<int>(rng() % allocationCount);
            const int va = t.allocations[a].modeDelay;
            const int vb = t.allocations[b].modeDelay;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < allocationCount; ++i)
        {
            shadow.allocations[i].modeDelay = t.allocations[i].modeDelay;
        }
        const optimaru::HardSoftScore full
            = ScoreCalculator::calculateEngineScore(shadow);
        const optimaru::HardSoftScore incremental = director.calculateScore();
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
    ProjectJobSchedule t = buildRandomInstance(/*projects*/ 2,
                                               /*standardPerProject*/ 4,
                                               /*seed*/ 11,
                                               /*delayRangeSize*/ 30);

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
        solver.solve(t);
        final = t.score;
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(t);
        check(final == recomputed, "solver-reported score == full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) A demo-sized solve reaches feasibility (hard 0), assigns every
// allocation and keeps the pinned SOURCE/SINK allocations untouched.
// -------------------------------------------------------------------------
void verifySolve()
{
    std::printf("== (d) Demo-sized solve ==\n");
    ProjectJobSchedule t = buildRandomInstance(/*projects*/ 2,
                                               /*standardPerProject*/ 10,
                                               /*seed*/ 21,
                                               /*delayRangeSize*/ 500);
    // Pin one STANDARD allocation to a specific value too.
    ScoreCalculator::prepare(t);
    const int pinnedStandard = 2;  // a STANDARD job of project 0
    const int pinnedValue = t.valueOf(3, 0);
    t.allocations[pinnedStandard].modeDelay = pinnedValue;
    t.allocations[pinnedStandard].pinned = true;

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(5);
    config.randomSeed = 5;
    // Early exit at feasibility keeps the test fast.
    config.bestScoreLimit = Score{0, -(1LL << 40), -(1LL << 40)};
    Solver solver(config);
    solver.solve(t);

    check(t.score.hard == 0, "demo-sized instance reaches 0hard");
    bool allAssigned = true;
    for (const Allocation &allocation : t.allocations)
    {
        if (allocation.modeDelay < 0)
            allAssigned = false;
    }
    check(allAssigned, "every allocation assigned after the solve");
    bool pinnedKept = t.allocations[pinnedStandard].modeDelay == pinnedValue;
    for (int a = 0; a < t.allocationCount(); ++a)
    {
        if (t.jobTypeOf(a) != JobType::STANDARD
            && t.allocations[a].modeDelay != 0)
        {
            pinnedKept = false;
        }
    }
    check(pinnedKept, "pinned allocations kept their (mode, delay)");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifySolve();

    if (failures == 0)
    {
        std::printf("All project-job-scheduling checks PASSED.\n");
        return 0;
    }
    std::printf("%d project-job-scheduling check(s) FAILED.\n", failures);
    return 1;
}

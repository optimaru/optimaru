// Project-job-scheduling unit tests (engine test/ style: check() + non-zero
// exit on any failure):
//   (a) every constraint verified against hand-computed scores (the
//       derived dates included), plus composite-encoding and pass-through
//       checks
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's Editor protocol (before/set/after)
//   (c) a small instance solved under ScoreChecks::ScoreChecks::Full
//   (d) a demo-sized solve reaches feasibility and keeps pinned allocations
//   (e) SerialScheduleMove: a hand-computed case; a walk through the engine's
//       move protocol (incremental == full recompute, undo restores, a
//       rebuilt schedule is within every renewable capacity, pinned
//       allocations keep their value); and a solve with the move ends ahead
//       of one without it
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "pjs/domain.hpp"
#include "pjs/incremental_calculator.hpp"
#include "pjs/moves.hpp"
#include "pjs/score_calculator.hpp"
#include "pjs/solver.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/testing/check.hpp"

using namespace pjs;

namespace {

using optimaru::testing::check;
using optimaru::testing::failures;

void checkScore(const std::string& label, const Score& actual, long expectedHard,
                long expectedMedium, long expectedSoft) {
    const bool ok = actual.hard == expectedHard && actual.medium == expectedMedium &&
                    actual.soft == expectedSoft;
    std::printf("  [%s] %-44s got=%s expected=%ldhard/%ldmedium/%ldsoft\n", ok ? "PASS" : "FAIL",
                label.c_str(), actual.str().c_str(), expectedHard, expectedMedium, expectedSoft);
    if (!ok) ++failures;
}

// One project, one resource; jobs/allocations appended by the helpers below.
ProjectJobSchedule makeBase(int resourceCapacity, bool renewable, int criticalPathDuration) {
    ProjectJobSchedule schedule;
    schedule.projects.push_back({"1", 0, criticalPathDuration});
    schedule.resources.push_back({"1", resourceCapacity, renewable, 0});
    return schedule;
}

// Adds a job with a single execution mode (duration, one optional
// requirement on resource 0) and its 1:1 allocation. Returns the allocation
// index.
int addJob(ProjectJobSchedule& schedule, JobType jobType, int duration,
           int requirement,  // <= 0: no resource requirement
           const std::vector<int>& predecessors,
           int delay = -1)  // < 0: unassigned
{
    Job job;
    job.id = std::to_string(schedule.jobs.size());
    job.project = 0;
    job.jobType = jobType;
    ExecutionMode mode;
    mode.id = "m" + job.id;
    mode.duration = duration;
    if (requirement > 0) mode.resourceRequirements.push_back({0, requirement});
    job.executionModes.push_back(mode);
    schedule.jobs.push_back(job);

    Allocation allocation;
    allocation.id = job.id;
    allocation.job = static_cast<int>(schedule.jobs.size()) - 1;
    allocation.predecessorAllocations = predecessors;
    schedule.allocations.push_back(allocation);
    const int index = schedule.allocationCount() - 1;
    if (delay >= 0) {
        // Single-mode jobs everywhere in these fixtures: maxModeCount == 1,
        // so the composite value is simply the delay.
        schedule.allocations[index].modeDelay = delay;
    }
    return index;
}

// -------------------------------------------------------------------------
// (a) Hand-computed constraint cases, checked against the full recompute
// (which also exercises the derived dates: a duration-1 / duration-2
// predecessor gives predecessorsDoneDate = 1 / 2).
// -------------------------------------------------------------------------
void verifyConstraints() {
    std::printf("== (a) Verifying each constraint in isolation ==\n");

    // hard 1: nonRenewableResourceCapacityCost — requirements 7 + 6 against
    // capacity 10 (dates are irrelevant): penalizes 13 - 10 = 3.
    {
        ProjectJobSchedule s = makeBase(10, /*renewable*/ false, 0);
        addJob(s, JobType::STANDARD, 10, 7, {}, 100);
        addJob(s, JobType::STANDARD, 10, 6, {}, 100);
        checkScore("nonRenewableResourceCapacityCost", ScoreCalculator::score(s), -3, 0, 0);
    }
    // hard 1: within capacity costs nothing.
    {
        ProjectJobSchedule s = makeBase(13, /*renewable*/ false, 0);
        addJob(s, JobType::STANDARD, 10, 7, {}, 100);
        addJob(s, JobType::STANDARD, 10, 6, {}, 100);
        checkScore("nonRenewableResourceCapacityCost within capacity", ScoreCalculator::score(s), 0,
                   0, 0);
    }
    // hard 2: renewableResourceCapacityCost — requirements 7 (days 101..110)
    // and 5 (days 102..111) against capacity 10: 9 overlap days x 2 = 18.
    // predecessorsDoneDate 1 / 2 comes from duration-1 / duration-2
    // predecessors.
    {
        ProjectJobSchedule s = makeBase(10, /*renewable*/ true, 0);
        const int p1 = addJob(s, JobType::STANDARD, 1, 0, {}, 0);
        const int p2 = addJob(s, JobType::STANDARD, 2, 0, {}, 0);
        const int a1 = addJob(s, JobType::STANDARD, 10, 7, {p1}, 100);
        const int a2 = addJob(s, JobType::STANDARD, 10, 5, {p2}, 100);
        checkScore("renewableResourceCapacityCost", ScoreCalculator::score(s), -18, 0, 0);
        // The derived dates (predecessorsDoneDate -> start -> end).
        check(s.allocations[a1].predecessorsDoneDate == 1 && s.allocations[a1].startDate == 101 &&
                      s.allocations[a1].endDate == 111,
              "derived dates of allocation 1 (done=1, start=101, end=111)");
        check(s.allocations[a2].predecessorsDoneDate == 2 && s.allocations[a2].startDate == 102 &&
                      s.allocations[a2].endDate == 112,
              "derived dates of allocation 2 (done=2, start=102, end=112)");
    }
    // medium 3 + soft 4 (+ hard 2): the project-delay case
    // (projectDelayCost + makespanCost) — SINK ends 111 and 112 against
    // criticalPathEndDate 100 -> medium 11 + 12 = 23; makespan
    // max(111, 112) = 112; the renewable overlap is 18 again.
    {
        ProjectJobSchedule s = makeBase(10, /*renewable*/ true, 100);
        const int p1 = addJob(s, JobType::STANDARD, 1, 0, {}, 0);
        const int p2 = addJob(s, JobType::STANDARD, 2, 0, {}, 0);
        addJob(s, JobType::SINK, 10, 7, {p1}, 100);
        addJob(s, JobType::SINK, 10, 5, {p2}, 100);
        checkScore("projectDelayCost + makespanCost", ScoreCalculator::score(s), -18, -23, -112);
    }
    // medium 3: no penalty when the sinks end before the critical path end
    // (the makespan case, makespanCost: criticalPathDuration 155).
    {
        ProjectJobSchedule s = makeBase(10, /*renewable*/ true, 155);
        const int p1 = addJob(s, JobType::STANDARD, 1, 0, {}, 0);
        const int p2 = addJob(s, JobType::STANDARD, 2, 0, {}, 0);
        addJob(s, JobType::SINK, 10, 7, {p1}, 100);
        addJob(s, JobType::SINK, 10, 5, {p2}, 100);
        checkScore("makespanCost (projectDelayCost 0)", ScoreCalculator::score(s), -18, 0, -112);
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
        ScoreCalculator::score(s);
        check(s.allocations[narrowAllocation].startDate == 4 &&
                      s.allocations[narrowAllocation].endDate == 7,
              "slot 2 of a 2-mode job decodes to mode 0 (start 4, end 7)");
        // delay 4, slot 1 -> mode 1 (dur 7).
        s.allocations[narrowAllocation].modeDelay = s.valueOf(4, 1);
        ScoreCalculator::score(s);
        check(s.allocations[narrowAllocation].endDate == 11,
              "slot 1 of a 2-mode job decodes to mode 1 (end 11)");
    }
    // Unassigned allocations pass dates through (delay 0 / duration 0) and
    // contribute no resource usage.
    {
        ProjectJobSchedule s = makeBase(1, /*renewable*/ true, 0);
        const int a = addJob(s, JobType::STANDARD, 2, 1, {}, 1);
        const int b = addJob(s, JobType::STANDARD, 5, 9, {a});  // unassigned
        const int c = addJob(s, JobType::STANDARD, 3, 1, {b}, 0);
        checkScore("unassigned contributes no usage", ScoreCalculator::score(s), 0, 0, 0);
        check(s.allocations[b].startDate == 3 && s.allocations[b].endDate == 3 &&
                      s.allocations[c].startDate == 3 && s.allocations[c].endDate == 6,
              "unassigned passes dates through (b 3-3, c 3-6)");
    }
    // Pinning: SOURCE/SINK are always pinned, STANDARD only when flagged.
    {
        ProjectJobSchedule s = makeBase(10, true, 0);
        const int source = addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        const int standard = addJob(s, JobType::STANDARD, 1, 1, {source}, 0);
        const int sink = addJob(s, JobType::SINK, 0, 0, {standard}, 0);
        using Descriptor = optimaru::SolutionTraits<pjs::ProjectJobSchedule>;
        check(Descriptor::is_pinned(s, source) && Descriptor::is_pinned(s, sink) &&
                      !Descriptor::is_pinned(s, standard),
              "SOURCE/SINK pinned, STANDARD not");
        s.allocations[standard].pinned = true;
        check(Descriptor::is_pinned(s, standard), "user-pinned STANDARD");
    }
    // prepare() rejects a precedence cycle.
    {
        ProjectJobSchedule s = makeBase(10, true, 0);
        const int a = addJob(s, JobType::STANDARD, 1, 0, {}, 0);
        const int b = addJob(s, JobType::STANDARD, 1, 0, {a}, 0);
        s.allocations[a].predecessorAllocations.push_back(b);
        bool threw = false;
        try {
            ScoreCalculator::prepare(s);
        } catch (const std::invalid_argument&) {
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
ProjectJobSchedule buildRandomInstance(int projectCount, int standardPerProject, std::uint64_t seed,
                                       int delayRangeSize) {
    std::mt19937_64 rng(seed);
    ProjectJobSchedule schedule;
    schedule.delayRangeSize = delayRangeSize;

    schedule.resources.push_back({"global", 12, true, -1});
    for (int p = 0; p < projectCount; ++p) {
        schedule.projects.push_back({"P" + std::to_string(p), 2 * p, 10 + 3 * p});
        schedule.resources.push_back({"ren" + std::to_string(p), 10, true, p});
        schedule.resources.push_back({"non" + std::to_string(p), 30, false, p});
    }

    for (int p = 0; p < projectCount; ++p) {
        const int base = static_cast<int>(schedule.jobs.size());
        const int count = standardPerProject + 2;
        const std::vector<int> pool = {0, 1 + 2 * p, 2 + 2 * p};
        for (int i = 0; i < count; ++i) {
            Job job;
            job.id = "J" + std::to_string(base + i);
            job.project = p;
            job.jobType = i == 0           ? JobType::SOURCE
                          : i == count - 1 ? JobType::SINK
                                           : JobType::STANDARD;
            if (job.jobType == JobType::STANDARD) {
                const int modeCount = 1 + static_cast<int>(rng() % 3);
                for (int m = 0; m < modeCount; ++m) {
                    ExecutionMode mode;
                    mode.id = job.id + "m" + std::to_string(m);
                    mode.duration = 1 + static_cast<int>(rng() % 5);
                    const int reqCount = 1 + static_cast<int>(rng() % 2);
                    for (int r = 0; r < reqCount; ++r) {
                        mode.resourceRequirements.push_back(
                                {pool[rng() % pool.size()], 1 + static_cast<int>(rng() % 5)});
                    }
                    job.executionModes.push_back(mode);
                }
            } else {
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
        for (int i = 1; i < count - 1; ++i) {
            schedule.allocations[base + i].predecessorAllocations.push_back(i <= 2 ? base
                                                                                   : base + i - 1);
            if (i >= 3 && rng() % 2 == 0) {
                schedule.allocations[base + i].predecessorAllocations.push_back(
                        base + 1 + static_cast<int>(rng() % (i - 1)));
            }
        }
        schedule.allocations[base + count - 1].predecessorAllocations.push_back(base + count - 2);
    }
    return schedule;
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves (including unassignments), driven through the Editor so
// the before/set/after hook contract is exercised exactly like in the
// engine. Every move can shift whole successor subgraphs, so this covers the
// re-propagation of the derived dates.
// -------------------------------------------------------------------------
void verifyIncremental() {
    std::printf("== (b) Verifying incremental scoring == full recompute ==\n");
    ProjectJobSchedule t = buildRandomInstance(/*projects*/ 2,
                                               /*standardPerProject*/ 6,
                                               /*seed*/ 7,
                                               /*delayRangeSize*/ 40);
    ScoreCalculator::prepare(t);

    ProjectJobSchedulingIncrementalScoreCalculator calc;
    optimaru::Editor<ProjectJobSchedule> editor(t, calc);
    ProjectJobSchedule shadow = t;

    std::mt19937_64 rng(123);
    const int allocationCount = t.allocationCount();
    const int value_count = t.value_count();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step) {
        if (unit(rng) < 0.75) {
            // ChangeMove; 10% of the time to unassigned (-1), like the ruin
            // phase and the construction heuristic's starting state.
            const int allocation = static_cast<int>(rng() % allocationCount);
            const int value = (unit(rng) < 0.10) ? -1 : static_cast<int>(rng() % value_count);
            editor.assign(allocation, value);
        } else {
            // SwapMove (also across jobs with different mode counts).
            const int a = static_cast<int>(rng() % allocationCount);
            const int b = static_cast<int>(rng() % allocationCount);
            const int va = t.allocations[a].modeDelay;
            const int vb = t.allocations[b].modeDelay;
            editor.assign(a, vb);
            editor.assign(b, va);
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < allocationCount; ++i) {
            shadow.allocations[i].modeDelay = t.allocations[i].modeDelay;
        }
        const optimaru::Score full = ScoreCalculator::calculateEngineScore(shadow);
        const optimaru::Score incremental = editor.score();
        if (full.hard != incremental.hard || std::fabs(full.soft - incremental.soft) > 1e-9) {
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
// (c) ScoreChecks::Full solve: the engine cross-checks the incremental score
// against a full reset + recompute after every single move and throws
// ScoreMismatchError on any divergence.
// -------------------------------------------------------------------------
void verifyFullAssertSolve() {
    std::printf("== (c) FULL_ASSERT solve ==\n");
    ProjectJobSchedule t = buildRandomInstance(/*projects*/ 2,
                                               /*standardPerProject*/ 4,
                                               /*seed*/ 11,
                                               /*delayRangeSize*/ 30);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(2);
    config.seed = 3;
    config.score_checks = optimaru::ScoreChecks::Full;
    // Kick early so the LNS ruin/recreate path also runs under ScoreChecks::Full.
    config.ruin_recreate_unimproved_step_trigger = 200;
    config.ruin_recreate_polish_steps = 100;

    bool corrupted = false;
    Score final{};
    try {
        Solver solver(config);
        solver.solve(t);
        final = t.score;
    } catch (const optimaru::ScoreMismatchError& e) {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT solve without score corruption");
    if (!corrupted) {
        const Score recomputed = ScoreCalculator::score(t);
        check(final == recomputed, "solver-reported score == full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) A demo-sized solve reaches feasibility (hard 0), assigns every
// allocation and keeps the pinned SOURCE/SINK allocations untouched.
// -------------------------------------------------------------------------
void verifySolve() {
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
    config.time_limit = std::chrono::seconds(5);
    config.seed = 5;
    // Early exit at feasibility keeps the test fast.
    config.target_score = Score{0, -(1LL << 40), -(1LL << 40)};
    Solver solver(config);
    solver.solve(t);

    check(t.score.hard == 0, "demo-sized instance reaches 0hard");
    bool allAssigned = true;
    for (const Allocation& allocation : t.allocations) {
        if (allocation.modeDelay < 0) allAssigned = false;
    }
    check(allAssigned, "every allocation assigned after the solve");
    bool pinnedKept = t.allocations[pinnedStandard].modeDelay == pinnedValue;
    for (int a = 0; a < t.allocationCount(); ++a) {
        if (t.jobTypeOf(a) != JobType::STANDARD && t.allocations[a].modeDelay != 0) {
            pinnedKept = false;
        }
    }
    check(pinnedKept, "pinned allocations kept their (mode, delay)");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) SerialScheduleMove
// -------------------------------------------------------------------------
// One step of the engine's move protocol around a MoveOperator.
struct MoveDriver {
    explicit MoveDriver(optimaru::Editor<ProjectJobSchedule>& editor)
        : ctx{editor, nearby, noLists, noLists} {}
    optimaru::Neighborhoods nearby;  // empty: no distance meter
    std::vector<int> noLists;        // Assignment: no element index
    optimaru::OperatorContext<ProjectJobSchedule> ctx;
    std::vector<int> entities;
};

std::vector<int> valuesOf(const ProjectJobSchedule& schedule) {
    std::vector<int> values;
    for (const Allocation& allocation : schedule.allocations)
        values.push_back(allocation.modeDelay);
    return values;
}

// The days on which a renewable resource is over its capacity, summed over
// the resources, from the dates of a full recompute.
int renewableExcess(ProjectJobSchedule& schedule) {
    ScoreCalculator::score(schedule);
    int excess = 0;
    for (std::size_t r = 0; r < schedule.resources.size(); ++r) {
        if (!schedule.resources[r].renewable) continue;
        std::vector<int> days;
        for (int a = 0; a < schedule.allocationCount(); ++a) {
            const Allocation& allocation = schedule.allocations[a];
            if (allocation.modeDelay < 0) continue;
            for (const ResourceRequirement& requirement :
                 schedule.executionModeOf(a, allocation.modeDelay).resourceRequirements) {
                if (requirement.resource != static_cast<int>(r)) continue;
                if (static_cast<int>(days.size()) < allocation.endDate)
                    days.resize(allocation.endDate, 0);
                for (int day = allocation.startDate; day < allocation.endDate; ++day) {
                    days[day] += requirement.requirement;
                }
            }
        }
        for (int used : days) excess += std::max(0, used - schedule.resources[r].capacity);
    }
    return excess;
}

void verifySerialScheduleMove() {
    std::printf("== (e) Serial schedule move ==\n");

    // By hand. Capacity 3; A lasts 3 days and B 2, and each takes 2 units,
    // so they cannot overlap. A waits 4 days for no reason: B [0, 2), A
    // [4, 7), the sink at 7. Whatever the move changes in the order, placing
    // both again as early as they fit ends at 5: B [0, 2) A [2, 5), or A
    // [0, 3) B [3, 5). The critical path is 3, so the project is 2 late.
    {
        ProjectJobSchedule s = makeBase(3, /*renewable*/ true, 3);
        const int source = addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        const int a = addJob(s, JobType::STANDARD, 3, 2, {source}, 4);
        const int b = addJob(s, JobType::STANDARD, 2, 2, {source}, 0);
        addJob(s, JobType::SINK, 0, 0, {a, b}, 0);
        ScoreCalculator::prepare(s);
        checkScore("by hand: A waits for no reason", ScoreCalculator::score(s), 0, -4, -7);

        bool ok = true;
        int applied = 0;
        for (std::uint64_t seed = 0; seed < 40; ++seed) {
            ProjectJobSchedule t = s;
            ProjectJobSchedulingIncrementalScoreCalculator calc;
            optimaru::Editor<ProjectJobSchedule> editor(t, calc);
            MoveDriver driver(editor);
            SerialScheduleMove move;
            std::mt19937_64 rng(seed);
            if (!move.apply(driver.ctx, rng, driver.entities)) {
                ok = ok && valuesOf(t) == valuesOf(s);  // declined: nothing touched
                continue;
            }
            ++applied;
            const Score rebuilt = ScoreCalculator::score(t);
            ok = ok && rebuilt.hard == 0 && rebuilt.medium == -2 && rebuilt.soft == -5;
            ok = ok && editor.score() == toEngineScore(rebuilt);
            move.undo_last(driver.ctx);
            ok = ok && valuesOf(t) == valuesOf(s) &&
                 editor.score() == toEngineScore(Score{0, -4, -7});
        }
        check(ok && applied >= 20, "by hand: every applied move ends at 5 within capacity (" +
                                           std::to_string(applied) +
                                           " of 40 draws applied); undo restores");
    }

    // The walk: three projects with several modes, a delay range that no
    // placement runs out of. Half the steps are single random changes, which
    // leave the schedule over capacity or with an allocation unassigned. The
    // second walk pins a STANDARD allocation: it keeps its mode and its
    // delay, and takes its resources where that puts it, so only the first
    // walk holds every rebuilt schedule to the capacities.
    for (const bool withPinned : {false, true}) {
        const std::string walk = withPinned ? "walk, one allocation pinned: " : "walk: ";
        ProjectJobSchedule t = buildRandomInstance(/*projects*/ 3,
                                                   /*standardPerProject*/ 8,
                                                   /*seed*/ 11,
                                                   /*delayRangeSize*/ 400);
        ScoreCalculator::prepare(t);
        if (withPinned) {
            const int pinnedStandard = 3;
            t.allocations[pinnedStandard].modeDelay = t.valueOf(2, 0);
            t.allocations[pinnedStandard].pinned = true;
        }
        for (int a = 0; a < t.allocationCount(); ++a) {
            if (t.allocations[a].modeDelay < 0) t.allocations[a].modeDelay = t.valueOf(a % 7, 0);
        }

        ProjectJobSchedulingIncrementalScoreCalculator calc;
        optimaru::Editor<ProjectJobSchedule> editor(t, calc);
        MoveDriver driver(editor);
        ProjectJobSchedule shadow = t;
        SerialScheduleMove move;
        std::mt19937_64 rng(31);
        const int count = t.allocationCount();
        using Traits = optimaru::SolutionTraits<ProjectJobSchedule>;

        auto agrees = [&]() {
            for (int i = 0; i < count; ++i)
                shadow.allocations[i].modeDelay = t.allocations[i].modeDelay;
            const optimaru::Score full = ScoreCalculator::calculateEngineScore(shadow);
            const optimaru::Score incremental = editor.score();
            return full.hard == incremental.hard && std::fabs(full.soft - incremental.soft) < 1e-9;
        };

        int applied = 0, kept = 0, undone = 0, declined = 0, declinedUnassigned = 0;
        int mismatches = 0, badUndo = 0, overCapacity = 0, pinnedMoved = 0, fromOverCapacity = 0;
        for (int step = 0; step < 4000; ++step) {
            if (rng() % 2 == 0) {
                // A single change; one in ten takes an allocation out, and
                // the next single change of an unassigned one puts it back.
                int allocation = static_cast<int>(rng() % count);
                for (int a = 0; a < count; ++a) {
                    if (t.allocations[a].modeDelay < 0) allocation = a;
                }
                if (Traits::is_pinned(t, allocation)) continue;
                const bool out = t.allocations[allocation].modeDelay >= 0 && rng() % 10 == 0;
                const int delay = static_cast<int>(rng() % 3);  // early, so that it often collides
                editor.assign(
                        allocation,
                        out ? -1 : t.valueOf(delay, static_cast<int>(rng() % t.maxModeCount)));
                if (!agrees()) ++mismatches;
                continue;
            }
            const std::vector<int> before = valuesOf(t);
            const optimaru::Score beforeScore = editor.score();
            bool unassigned = false;
            for (int value : before) unassigned = unassigned || value < 0;
            const bool startedOver = renewableExcess(shadow) > 0;
            driver.entities.clear();
            if (!move.apply(driver.ctx, rng, driver.entities)) {
                ++declined;
                if (unassigned) ++declinedUnassigned;
                if (valuesOf(t) != before) ++badUndo;
                continue;
            }
            ++applied;
            if (unassigned) ++badUndo;  // the move does nothing inside a ruin
            if (startedOver) ++fromOverCapacity;
            if (!agrees()) ++mismatches;
            if (renewableExcess(shadow) > 0) ++overCapacity;
            for (int a = 0; a < count; ++a) {
                if (Traits::is_pinned(t, a) && t.allocations[a].modeDelay != before[a])
                    ++pinnedMoved;
            }
            if (rng() % 2 == 0) {
                move.undo_last(driver.ctx);
                ++undone;
                if (valuesOf(t) != before || !(editor.score() == beforeScore)) ++badUndo;
            } else {
                ++kept;
            }
            if (!agrees()) ++mismatches;
        }
        check(mismatches == 0, walk + std::to_string(applied) + " applied (" +
                                       std::to_string(kept) + " kept, " + std::to_string(undone) +
                                       " undone), incremental == full recompute");
        check(applied > 800 && kept > 300 && undone > 300 && fromOverCapacity > 50,
              walk + "the move is applied and undone often, from schedules over capacity too (" +
                      std::to_string(fromOverCapacity) + ")");
        check(declinedUnassigned > 50 && declined >= declinedUnassigned,
              walk + "the move declines while an allocation is unassigned (" +
                      std::to_string(declinedUnassigned) + " times)");
        check(badUndo == 0, walk + "undo (and a declined move) leaves every value as it was");
        if (!withPinned) {
            check(overCapacity == 0,
                  walk + "a rebuilt schedule is within every renewable capacity");
        }
        check(pinnedMoved == 0, walk + "pinned allocations keep their value");
    }

    // A solve. 40 activities of one project over two resources: the same
    // seed and the same number of steps with and without the move. The
    // margin is wide (the built-in moves stall well above what the order
    // allows), so the comparison does not hang on one draw.
    {
        auto solved = [](bool moves) {
            ProjectJobSchedule t = buildRandomInstance(/*projects*/ 1,
                                                       /*standardPerProject*/ 40,
                                                       /*seed*/ 5,
                                                       /*delayRangeSize*/ 400);
            Solver::Config config;
            config.step_limit = 30000;
            config.seed = 3;
            config.serial_schedule_moves = moves;
            Solver(config).solve(t);
            return t.score;
        };
        const Score with = solved(true);
        const Score without = solved(false);
        std::printf("  with the move %s, without %s\n", with.str().c_str(), without.str().c_str());
        check(with.hard == 0 && with.medium > without.medium,
              "solve: 30,000 steps with the move end ahead of 30,000 without it");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// stop_at_first_feasible's best-score limit (0, INT64_MIN, INT64_MIN): the
// engine compares hard only (soft -inf), so no soft score, however low,
// misses the stop. Through toEngineScore the sentinel folded into a finite
// engine soft that a soft score below it never reached.
// -------------------------------------------------------------------------
void verifyStopAtFirstFeasibleLimit() {
    std::printf("== stop_at_first_feasible's limit is hard-only ==\n");
    const optimaru::Score limit = toEngineScoreLimit(Score{0, kNoLevelFloor, kNoLevelFloor});
    check(limit.hard == 0 && std::isinf(limit.soft) && limit.soft < 0.0,
          "(0, INT64_MIN, INT64_MIN) -> engine limit (0, -inf)");
    check(optimaru::Score{0, -1e30} >= limit && toEngineScore(Score{0, -5, -(1LL << 62)}) >= limit,
          "a feasible score with any soft (-1e30; medium -5, soft -2^62) reaches it");
    check(!(optimaru::Score{-1, 0.0} >= limit), "an infeasible score does not");
    check(toEngineScoreLimit(Score{0, kNoLevelFloor, 0}).soft ==
                          -std::numeric_limits<double>::infinity() &&
                  toEngineScoreLimit(Score{0, 0, kNoLevelFloor}).soft ==
                          -std::numeric_limits<double>::infinity(),
          "INT64_MIN on medium or on soft drops the soft-side floor");
    const Score other{-2, -3, -5};
    check(toEngineScoreLimit(other) == toEngineScore(other), "any other limit folds like a score");
    std::printf("\n");
}

}  // namespace

int main() {
    verifyStopAtFirstFeasibleLimit();
    verifyConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifySolve();
    verifySerialScheduleMove();

    if (failures == 0) {
        std::printf("All project-job-scheduling checks PASSED.\n");
        return 0;
    }
    std::printf("%d project-job-scheduling check(s) FAILED.\n", failures);
    return 1;
}

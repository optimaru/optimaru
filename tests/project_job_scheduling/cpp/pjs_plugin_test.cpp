// Plugin-assembly (pjs/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedObjective
//       reproduces the monolithic score bit-for-bit on 300 random
//       assignments (unassigned allocations included)
//   (b) incremental score == full recompute across 1500 random moves driven
//       through the engine's Editor protocol (plugin path)
//   (c) partial assemblies: each component in isolation, hand-computed
//   (d) weight / level / exponent overrides
//   (e) constraint_totals() explains exactly the reported score
//   (f) a 2-second solve(schedule, calculator) under ScoreChecks::Full
//   (g) raw costs by hand through constraint_costs(): one positive and one
//       zero case per constraint
//   (h) the catalog (info()) equals the components' constants; level and
//       weight guards
//   (i) the medium level (projectDelayCost): the default fold, hard(),
//       soft() and an explicit weight; solve() reports the assembly's levels
//   (j) incremental == full recompute with the medium term overridden
//   (k) repeated components and the offset / clip knobs: a hand-calculated
//       repeat, threshold and literal offsets by hand on the global totals
//       (each shaped once), the literal formula refused on hard at reset and
//       kept on soft (and on the into_soft medium), incremental == a fresh
//       recompute over a random walk and a ScoreChecks::Full solve with repeated,
//       shifted components
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
#include <utility>
#include <vector>

#include "pjs/constraints.hpp"
#include "pjs/domain.hpp"
#include "pjs/score_calculator.hpp"
#include "pjs/solver.hpp"
#include "optimaru/model/editor.hpp"

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
                actual.str().c_str(),
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
    calculator.rebuild(s);
    return fromEngineScore(calculator.score());
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
                                   : static_cast<int>(rng() % s.value_count());
        }
        const Score plugin = scoreWith(calculator, s);
        const Score monolithic = ScoreCalculator::score(s);
        if (!(plugin == monolithic))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n",
                            round,
                            plugin.str().c_str(),
                            monolithic.str().c_str());
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
    optimaru::Editor<ProjectJobSchedule> editor(s, calculator);
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
                                  : static_cast<int>(rng() % s.value_count());
            editor.assign(a, value);
        }
        else
        {
            const int a = movable[rng() % movable.size()];
            const int b = movable[rng() % movable.size()];
            const int va = s.allocations[a].modeDelay;
            const int vb = s.allocations[b].modeDelay;
            editor.assign(a, vb);
            editor.assign(b, va);
        }
        for (int i = 0; i < s.allocationCount(); ++i)
            shadow.allocations[i].modeDelay = s.allocations[i].modeDelay;
        const Score full = ScoreCalculator::score(shadow);
        const Score incremental = fromEngineScore(editor.score());
        if (!(full == incremental))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n",
                            step,
                            incremental.str().c_str(),
                            full.str().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps)
              + " random moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Single-component assemblies.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Single-component assemblies ==\n");

    {  // nonRenewableResourceCapacityCost alone: 7 + 6 against a capacity of
       // 10.
        ProjectJobSchedule s = makeBase(10, /*renewable*/ false, 0);
        addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        addJob(s, JobType::STANDARD, 1, 7, {0}, 0);
        addJob(s, JobType::STANDARD, 1, 6, {0}, 0);
        DecomposedCalculator calculator;
        calculator.emplace<NonRenewableResourceCapacityCost>();
        checkScore("nonRenewableResourceCapacityCost alone",
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
        calculator.emplace<NonRenewableResourceCapacityCost>();
        checkScore("nonRenewableResourceCapacityCost within capacity",
                   scoreWith(calculator, s),
                   0,
                   0,
                   0);
    }
    {  // renewableResourceCapacityCost alone: two overlapping jobs needing
        // 7 + 6 on one day against a capacity of 10.
        ProjectJobSchedule s = makeBase(10, /*renewable*/ true, 0);
        addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        addJob(s, JobType::STANDARD, 1, 7, {0}, 0);
        addJob(s, JobType::STANDARD, 1, 6, {0}, 0);
        DecomposedCalculator calculator;
        calculator.emplace<RenewableResourceCapacityCost>();
        checkScore("renewableResourceCapacityCost alone",
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
        calculator.emplace<RenewableResourceCapacityCost>();
        checkScore("renewableResourceCapacityCost on separate days",
                   scoreWith(calculator, s),
                   0,
                   0,
                   0);
    }
    {  // projectDelayCost alone: a sink ending on day 3 against a critical
        // path end date of 1.
        ProjectJobSchedule s = makeBase(100, true, /*criticalPathDuration*/ 1);
        addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        addJob(s, JobType::STANDARD, 3, 0, {0}, 0);
        addJob(s, JobType::SINK, 0, 0, {1}, 0);
        DecomposedCalculator calculator;
        calculator.emplace<ProjectDelayCost>();
        checkScore(
            "projectDelayCost alone", scoreWith(calculator, s), 0, -2, 0);
    }
    {  // makespanCost alone: the same schedule's last sink end date.
        ProjectJobSchedule s = makeBase(100, true, /*criticalPathDuration*/ 1);
        addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        addJob(s, JobType::STANDARD, 3, 0, {0}, 0);
        addJob(s, JobType::SINK, 0, 0, {1}, 0);
        DecomposedCalculator calculator;
        calculator.emplace<MakespanCost>();
        checkScore("makespanCost alone", scoreWith(calculator, s), 0, 0, -3);
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
        calculator.emplace<NonRenewableResourceCapacityCost>(10.0);
        checkScore("nonRenewableResourceCapacityCost re-weighted to 10",
                   scoreWith(calculator, s),
                   -30,
                   0,
                   0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<NonRenewableResourceCapacityCost>();
        component->level = optimaru::Level::Soft;
        calculator.add(std::move(component));
        checkScore("nonRenewableResourceCapacityCost demoted to soft",
                   scoreWith(calculator, s),
                   0,
                   0,
                   -3);
    }
    {
        // exponent 2 shapes the raw excess (3) before the weight.
        DecomposedCalculator calculator;
        auto component = std::make_unique<NonRenewableResourceCapacityCost>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("nonRenewableResourceCapacityCost with exponent 2",
                   scoreWith(calculator, s),
                   -9,
                   0,
                   0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraint_totals(): the per-constraint breakdown adds up.
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
                = static_cast<int>(rng() % s.value_count());
    }

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(s);
    const optimaru::Score score = calculator.score();

    // A medium total is in medium units (cost is not multiplied by the
    // fold); fold it here to compare with the engine score.
    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraint_totals())
    {
        if (total.level == optimaru::Level::Hard)
            hard += total.cost;
        else if (total.level == optimaru::Level::Medium)
            soft += total.cost * static_cast<double>(kMediumUnit);
        else
            soft += total.cost;
    }
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-3,
          "per-constraint totals sum to the reported score");
    check(calculator.constraint_totals().size() == 4,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) ScoreChecks::Full solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    ProjectJobSchedule s = buildRandomInstance(/*projects*/ 2,
                                               /*standardPerProject*/ 4,
                                               /*seed*/ 11,
                                               /*delayRangeSize*/ 8);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(2);
    config.seed = 3;
    config.score_checks = optimaru::ScoreChecks::Full;
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
    catch (const optimaru::ScoreMismatchError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::score(s);
        check(s.score == recomputed,
              "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) Raw costs by hand, read through constraint_costs() (unweighted, before
// level and exponent): every constraint once positive, once zero.
// -------------------------------------------------------------------------

// One project (release 0) with a renewable resource "ren" (index 0) and a
// non-renewable resource "non" (index 1): SOURCE -> {A, B} -> SINK, every
// allocation at delay 0 (single-mode jobs, so the value is the delay).
//   A: 2 days, needs ren 7 and non 8;  B: 1 day, needs ren 6 and non 6.
// Dates: A days [0, 2), B day [0, 1), the SINK ends on day 2.
ProjectJobSchedule handSchedule(int renewableCapacity,
                                int nonRenewableCapacity,
                                int criticalPathDuration)
{
    ProjectJobSchedule schedule;
    schedule.projects.push_back({"P", 0, criticalPathDuration});
    schedule.resources.push_back({"ren", renewableCapacity, true, 0});
    schedule.resources.push_back({"non", nonRenewableCapacity, false, 0});
    const auto job = [&schedule](JobType type,
                                 int duration,
                                 std::vector<ResourceRequirement> needs,
                                 std::vector<int> predecessors)
    {
        Job j;
        j.id = std::to_string(schedule.jobs.size());
        j.project = 0;
        j.jobType = type;
        j.executionModes.push_back({"m" + j.id, duration, std::move(needs)});
        schedule.jobs.push_back(j);
        Allocation allocation;
        allocation.id = j.id;
        allocation.job = static_cast<int>(schedule.jobs.size()) - 1;
        allocation.predecessorAllocations = std::move(predecessors);
        allocation.modeDelay = 0;
        schedule.allocations.push_back(allocation);
        return schedule.allocationCount() - 1;
    };
    const int source = job(JobType::SOURCE, 0, {}, {});
    const int a = job(JobType::STANDARD, 2, {{0, 7}, {1, 8}}, {source});
    const int b = job(JobType::STANDARD, 1, {{0, 6}, {1, 6}}, {source});
    job(JobType::SINK, 0, {}, {a, b});
    return schedule;
}

std::vector<double> rawCosts(ProjectJobSchedule &schedule,
                             const std::string &name)
{
    ScoreCalculator::prepare(schedule);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(schedule);
    calculator.score();
    for (const auto &entry : calculator.constraint_costs())
    {
        if (entry.name == name)
            return entry.unit_costs;
    }
    return {};
}

void verifyRawCostsByHand()
{
    std::printf("== (g) Raw costs by hand (constraintCosts) ==\n");
    using V = std::vector<double>;
    {
        // ren 10 per day: day 0 carries 7 + 6 = 13 -> 3, day 1 carries 7 -> 0.
        // non 10 in all: 8 + 6 = 14 -> 4. Critical path end 0 + 1 = 1, the
        // SINK ends on day 2 -> 1 day late. Makespan: the SINK ends on day 2.
        ProjectJobSchedule s = handSchedule(10, 10, 1);
        check(rawCosts(s, "nonRenewableResourceCapacityCost") == V{4.0},
              "nonRenewableResourceCapacityCost positive: 8 + 6 - 10 = 4");
        check(rawCosts(s, "renewableResourceCapacityCost") == V{3.0},
              "renewableResourceCapacityCost positive: day 0, 13 - 10 = 3");
        check(rawCosts(s, "projectDelayCost") == V{1.0},
              "projectDelayCost positive: SINK end 2 - critical path end 1");
        check(rawCosts(s, "makespanCost") == V{2.0},
              "makespanCost positive: the SINK ends on day 2");
        checkScore("the hand schedule at the default weights",
                   ScoreCalculator::score(s),
                   -7,
                   -1,
                   -2);
    }
    {
        // Capacities 13 / 14 hold exactly; critical path end 2 == SINK end.
        ProjectJobSchedule s = handSchedule(13, 14, 2);
        check(rawCosts(s, "nonRenewableResourceCapacityCost") == V{0.0},
              "nonRenewableResourceCapacityCost zero: 14 within 14");
        check(rawCosts(s, "renewableResourceCapacityCost") == V{0.0},
              "renewableResourceCapacityCost zero: 13 within 13 on day 0");
        check(rawCosts(s, "projectDelayCost") == V{0.0},
              "projectDelayCost zero: the SINK ends on its critical path end");
    }
    {
        // Only zero-duration milestones: the last project ends on day 0.
        ProjectJobSchedule s = makeBase(10, true, 0);
        addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        addJob(s, JobType::SINK, 0, 0, {0}, 0);
        check(rawCosts(s, "makespanCost") == V{0.0},
              "makespanCost zero: every SINK ends on day 0");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) The catalog read off the default assembly is exactly the components'
// constants, and the level/weight guards hold on the domain's assembly.
// -------------------------------------------------------------------------
bool throwsInvalid(const std::function<void()> &edit)
{
    try
    {
        edit();
    }
    catch (const std::invalid_argument &)
    {
        return true;
    }
    return false;
}

template <typename Component>
bool rowIs(const optimaru::ConstraintInfo &row,
           const char *name,
           optimaru::Level level,
           double weight)
{
    return row.name == name && row.default_level == Component::kDefaultLevel
           && row.default_weight == Component::kDefaultWeight
           && row.default_level == level && row.default_weight == weight
           && row.level_flippable && !row.reward && row.is_global
           && row.default_enabled;
}

void verifyCatalog()
{
    std::printf("== (h) Catalog and guards ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 4, "4 catalog rows");
    if (rows.size() != 4)
        return;
    check(rowIs<NonRenewableResourceCapacityCost>(
              rows[0], "nonRenewableResourceCapacityCost", Level::Hard, 1.0)
              && rowIs<RenewableResourceCapacityCost>(
                  rows[1], "renewableResourceCapacityCost", Level::Hard, 1.0)
              && rowIs<ProjectDelayCost>(
                  rows[2], "projectDelayCost", Level::Medium, 1.0)
              && rowIs<MakespanCost>(rows[3], "makespanCost", Level::Soft, 1.0),
          "info(): nonRenewableResourceCapacityCost hard 1, "
          "renewableResourceCapacityCost hard 1, projectDelayCost medium 1, "
          "makespanCost soft 1; all global, flippable, no reward, enabled");
    std::vector<std::string> infoNames;
    for (const auto &row : rows)
        infoNames.push_back(row.name);
    check(infoNames == defaultConstraints().names(),
          "info() rows in names() order");
    check(!throwsInvalid(
              []
              { defaultConstraints().soft("nonRenewableResourceCapacityCost"); })
              && !throwsInvalid([]
                                { defaultConstraints().hard("makespanCost"); })
              && !throwsInvalid(
                  [] { defaultConstraints().hard("projectDelayCost"); })
              && !throwsInvalid(
                  [] { defaultConstraints().soft("projectDelayCost"); }),
          "every level flips (medium to hard or soft too)");
    check(throwsInvalid([]
                        { defaultConstraints().weight("makespanCost", -2.0); }),
          "a negative weight is refused");
    check(throwsInvalid(
              []
              {
                  defaultConstraints().exponent(
                      "renewableResourceCapacityCost", 0.0);
              }),
          "a non-positive exponent is refused");
    check(throwsInvalid(
              [] { defaultConstraints().weight("noSuchConstraint", 2.0); })
              && throwsInvalid([]
                               { defaultConstraints().hard("noSuchCost"); }),
          "an unknown registry name is refused");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The medium level: projectDelayCost.
// -------------------------------------------------------------------------
using LevelScores = DecomposedCalculator::LevelScores;

LevelScores levelsWith(ConstraintAssembly assembly,
                       ProjectJobSchedule &schedule)
{
    ScoreCalculator::prepare(schedule);
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    calculator.rebuild(schedule);
    calculator.score();
    return calculator.level_scores();
}

bool levelsAre(const LevelScores &levels,
               std::int64_t hard,
               double medium,
               double soft)
{
    return levels.hard == hard && levels.medium == medium
           && levels.soft == soft;
}

void verifyMediumLevel()
{
    std::printf("== (i) Medium level ==\n");
    // The (g) hand schedule: raw 4 + 3 hard, 1 day of project delay, makespan
    // 2 -> 7hard/-1medium/-2soft.
    ProjectJobSchedule s = handSchedule(10, 10, 1);
    const Score monolithic = ScoreCalculator::score(s);
    {
        ScoreCalculator::prepare(s);
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        calculator.rebuild(s);
        const optimaru::Score engine = calculator.score();
        check(engine.hard == -7
                  && engine.soft
                         == -2.0 - 1.0 * static_cast<double>(kMediumUnit),
              "default: medium folded into soft x 10^9");
        const optimaru::Score adapter = toEngineScore(monolithic);
        check(engine.hard == adapter.hard && engine.soft == adapter.soft,
              "default: the fold equals the monolithic Score adapter");
        check(levelsAre(calculator.level_scores(), -7, -1.0, -2.0),
              "levelScores() reports the unfolded levels");
    }
    check(levelsAre(levelsWith(defaultConstraints().hard("projectDelayCost"), s),
                    -8,
                    0.0,
                    -2.0),
          "projectDelayCost hard(): one hard unit per raw day");
    check(levelsAre(levelsWith(defaultConstraints().soft("projectDelayCost"), s),
                    -7,
                    0.0,
                    -3.0),
          "projectDelayCost soft(): one soft unit per raw day, outside the "
          "fold");
    check(levelsAre(
              levelsWith(defaultConstraints().weight("projectDelayCost", 3), s),
              -7,
              -3.0,
              -2.0),
          "projectDelayCost weight 3 stays in medium units");
    check(levelsAre(levelsWith(defaultConstraints().level(
                                   "makespanCost", optimaru::Level::Medium),
                               s),
                    -7,
                    -3.0,
                    0.0),
          "level(Medium): makespanCost moves onto medium");
    check(levelsAre(levelsWith(defaultConstraints()
                                   .level("makespanCost", optimaru::Level::Medium)
                                   .weight("makespanCost", 2),
                               s),
                    -7,
                    -5.0,
                    0.0),
          "a weight on a moved constraint is in medium units");
    check(levelsAre(levelsWith(defaultConstraints().level(
                                   "renewableResourceCapacityCost",
                                   optimaru::Level::Medium),
                               s),
                    -4,
                    -4.0,
                    -2.0),
          "level(Medium): a hard constraint moves onto medium too");
    {
        // solve(schedule, calculator) reports the assembly's levels. One
        // 3-day job between the milestones, critical path 1 day: at its best
        // (delay 0) the SINK ends on day 3, 2 days late. With
        // projectDelayCost demoted to soft those 2 days land on SOFT (not
        // medium), next to the makespan 3.
        ProjectJobSchedule one = makeBase(10, true, /*criticalPathDuration*/ 1);
        one.delayRangeSize = 5;
        addJob(one, JobType::SOURCE, 0, 0, {}, 0);
        addJob(one, JobType::STANDARD, 3, 0, {0});
        addJob(one, JobType::SINK, 0, 0, {1}, 0);
        Solver::Config config;
        config.time_limit = std::chrono::milliseconds(300);
        config.seed = 1;
        DecomposedCalculator calculator;
        defaultConstraints().soft("projectDelayCost").into(calculator);
        Solver(config).solve(one, calculator);
        checkScore("solve() with projectDelayCost demoted",
                   one.score,
                   0,
                   0,
                   -5);
    }
    {
        // on_new_best reports the calculator's levels like the final score, not
        // the decoded engine fold: demoted to soft at weight 10^9, the 2 days
        // of delay make a soft total of 2 * 10^9 + 3, which a decoded fold
        // would file as -2 medium / -3 soft.
        ProjectJobSchedule one = makeBase(10, true, /*criticalPathDuration*/ 1);
        one.delayRangeSize = 5;
        addJob(one, JobType::SOURCE, 0, 0, {}, 0);
        addJob(one, JobType::STANDARD, 3, 0, {0});
        addJob(one, JobType::SINK, 0, 0, {1}, 0);
        std::vector<Score> reported;
        Solver::Config config;
        config.time_limit = std::chrono::milliseconds(300);
        config.seed = 1;
        config.on_new_best = [&reported](std::int64_t, const Score &best)
        { reported.push_back(best); };
        DecomposedCalculator calculator;
        defaultConstraints()
            .soft("projectDelayCost")
            .weight("projectDelayCost", static_cast<double>(kMediumUnit))
            .into(calculator);
        Solver(config).solve(one, calculator);
        checkScore("solve() with projectDelayCost demoted at weight 10^9",
                   one.score,
                   0,
                   0,
                   -(2 * kMediumUnit + 3));
        bool noMedium = !reported.empty();
        for (const Score &best : reported)
            noMedium = noMedium && best.medium == 0;
        check(noMedium, "onNewBest never files the soft total under medium");
        check(!reported.empty() && reported.back() == one.score,
              "the last onNewBest score equals the final schedule.score");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) Incremental == full recompute with the medium term overridden,
// through the Editor protocol (changes, swaps, unassignments).
// -------------------------------------------------------------------------
void verifyMediumIncremental()
{
    std::printf("== (j) Medium overrides: incremental == full ==\n");
    const std::vector<
        std::pair<std::string, std::function<ConstraintAssembly()>>>
        variants{
            {"projectDelayCost hard",
             [] { return defaultConstraints().hard("projectDelayCost"); }},
            {"projectDelayCost soft",
             [] { return defaultConstraints().soft("projectDelayCost"); }},
            {"projectDelayCost weight 3",
             [] { return defaultConstraints().weight("projectDelayCost", 3); }},
            {"makespanCost on medium",
             []
             {
                 return defaultConstraints().level("makespanCost",
                                                   optimaru::Level::Medium);
             }},
        };
    for (const auto &[label, assembly] : variants)
    {
        ProjectJobSchedule s = buildRandomInstance(/*projects*/ 2,
                                                   /*standardPerProject*/ 5,
                                                   /*seed*/ 17,
                                                   /*delayRangeSize*/ 10);
        ScoreCalculator::prepare(s);
        DecomposedCalculator calculator;
        assembly().into(calculator);
        optimaru::Editor<ProjectJobSchedule> editor(s, calculator);
        ProjectJobSchedule shadow = s;

        std::mt19937_64 rng(31);
        std::vector<int> movable;
        for (int a = 0; a < s.allocationCount(); ++a)
        {
            if (s.jobTypeOf(a) == JobType::STANDARD)
                movable.push_back(a);
        }
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        int mismatches = 0;
        bool sawMedium = false;
        const int steps = 400;
        for (int step = 0; step < steps; ++step)
        {
            const int a = movable[rng() % movable.size()];
            if (unit(rng) < 0.75)
            {
                editor.assign(
                    a,
                    unit(rng) < 0.10
                        ? -1
                        : static_cast<int>(rng() % s.value_count()));
            }
            else
            {
                const int b = movable[rng() % movable.size()];
                const int va = s.allocations[a].modeDelay;
                const int vb = s.allocations[b].modeDelay;
                editor.assign(a, vb);
                editor.assign(b, va);
            }
            for (int i = 0; i < s.allocationCount(); ++i)
                shadow.allocations[i].modeDelay = s.allocations[i].modeDelay;
            DecomposedCalculator fresh;
            assembly().into(fresh);
            fresh.rebuild(shadow);
            const optimaru::Score full = fresh.score();
            const optimaru::Score incremental
                = editor.score();
            const LevelScores levels = calculator.level_scores();
            const LevelScores fullLevels = fresh.level_scores();
            sawMedium = sawMedium || levels.medium != 0.0;
            if (!(full == incremental) || levels.hard != fullLevels.hard
                || levels.medium != fullLevels.medium
                || levels.soft != fullLevels.soft)
            {
                ++mismatches;
            }
        }
        check(mismatches == 0,
              label + ": " + std::to_string(steps)
                  + " random moves, incremental == full recompute");
        if (label == "projectDelayCost weight 3" || label == "makespanCost on medium")
            check(sawMedium, label + ": the walk scores a medium term");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (k) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with add_instances(), as the Python bindings
// build it from a pick list. All four components are global: the exponent,
// offset and clip shape each instance's one total.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string &name)
{
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

ConstraintAssembly repeatedAssembly()
{
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("makespanCost", 3.0);
    assembly.add_instances(std::move(copyOf("makespanCost")
                                        .weight("makespanCost", 2.0)
                                        .offset("makespanCost", 10.0)
                                        .clip("makespanCost", false)));
    // The literal renewable copy sits on soft: hard refuses the literal
    // formula with an offset (hard 0 must mean no violation).
    assembly.add_instances(
        std::move(copyOf("renewableResourceCapacityCost")
                      .soft("renewableResourceCapacityCost")
                      .weight("renewableResourceCapacityCost", 2.0)
                      .offset("renewableResourceCapacityCost", 1.0)
                      .clip("renewableResourceCapacityCost", false)));
    assembly.add_instances(
        std::move(copyOf("renewableResourceCapacityCost")
                      .weight("renewableResourceCapacityCost", 2.0)
                      .offset("renewableResourceCapacityCost", 1.0)));
    assembly.add_instances(std::move(copyOf("projectDelayCost")
                                        .exponent("projectDelayCost", 2.0)
                                        .offset("projectDelayCost", 1.0)));
    assembly.add_instances(std::move(copyOf("projectDelayCost")
                                        .soft("projectDelayCost")
                                        .weight("projectDelayCost", 3.0)
                                        .offset("projectDelayCost", -2.0)
                                        .clip("projectDelayCost", false)));
    assembly.add_instances(
        std::move(copyOf("nonRenewableResourceCapacityCost")
                      .offset("nonRenewableResourceCapacityCost", -1.0)));
    return assembly;
}

void verifyRepeatsAndOffsets()
{
    std::printf("== (k) Repeated components, offset / clip ==\n");
    // The (g) hand schedule: raw non-renewable 4, renewable 3 (hard), project
    // delay 1 (medium), makespan 2 (soft).
    {
        ProjectJobSchedule s = handSchedule(10, 10, 1);
        ScoreCalculator::prepare(s);
        ConstraintAssembly twice = copyOf("makespanCost");
        twice.add_instances(std::move(copyOf("makespanCost")
                                         .weight("makespanCost", 2.0)
                                         .exponent("makespanCost", 2.0)));
        DecomposedCalculator calculator;
        std::move(twice).into(calculator);
        calculator.rebuild(s);
        calculator.score();
        check(levelsAre(calculator.level_scores(), 0, 0.0, -(2.0 + 2.0 * 4)),
              "makespan + 2 x makespan^2: -10 soft");
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].unit_costs == entries[1].unit_costs
                  && entries[0].unit_costs == std::vector<double>{2.0}
                  && entries[0].is_global && entries[1].is_global
                  && entries[1].weight == 2.0 && entries[1].exponent == 2.0
                  && entries[1].default_weight == 1.0,
              "two global instances, the same total, each with its own knobs");
    }
    {
        ProjectJobSchedule s = handSchedule(10, 10, 1);
        check(levelsAre(levelsWith(copyOf("renewableResourceCapacityCost")
                                       .offset("renewableResourceCapacityCost", 1.0),
                                   s),
                        -2,
                        0.0,
                        0.0),
              "renewable threshold offset 1: (3 - 1) once");
        check(levelsAre(levelsWith(copyOf("renewableResourceCapacityCost")
                                       .soft("renewableResourceCapacityCost")
                                       .offset("renewableResourceCapacityCost", 5.0)
                                       .clip("renewableResourceCapacityCost", false),
                                   s),
                        0,
                        0.0,
                        2.0),
              "renewable literal offset 5 on soft: (3 - 5) once, +2 soft");
        check(levelsAre(levelsWith(copyOf("nonRenewableResourceCapacityCost")
                                       .offset("nonRenewableResourceCapacityCost", -1.0),
                                   s),
                        -5,
                        0.0,
                        0.0),
              "non-renewable threshold offset -1: 4 + 1");
        check(levelsAre(levelsWith(copyOf("nonRenewableResourceCapacityCost")
                                       .weight("nonRenewableResourceCapacityCost", 3.0)
                                       .offset("nonRenewableResourceCapacityCost", 3.5),
                                   s),
                        -2,
                        0.0,
                        0.0),
              "non-renewable threshold offset 3.5 at weight 3: 1.5 -> 2 hard");
        check(levelsAre(levelsWith(copyOf("nonRenewableResourceCapacityCost")
                                       .soft("nonRenewableResourceCapacityCost")
                                       .weight("nonRenewableResourceCapacityCost", 3.0)
                                       .offset("nonRenewableResourceCapacityCost", 3.5)
                                       .clip("nonRenewableResourceCapacityCost", false),
                                   s),
                        0,
                        0.0,
                        -1.5),
              "non-renewable literal offset 3.5 at weight 3 on soft: -1.5 soft (not rounded)");
        check(levelsAre(levelsWith(copyOf("projectDelayCost")
                                       .offset("projectDelayCost", 3.0)
                                       .clip("projectDelayCost", false),
                                   s),
                        0,
                        2.0,
                        0.0),
              "project delay literal offset 3: (1 - 3), +2 medium");
        check(levelsAre(levelsWith(copyOf("makespanCost")
                                       .offset("makespanCost", 1.0)
                                       .exponent("makespanCost", 3.0),
                                   s),
                        0,
                        0.0,
                        -1.0),
              "makespan threshold offset 1, exponent 3: (2 - 1)^3");
    }
    {
        // On hard (the default level of both resource constraints) the literal
        // formula with an offset is refused at reset, whichever sign and
        // setter order: +5 would turn the renewable overload of 3 into a +2
        // hard reward, -1 would charge a schedule without any overload.
        auto resetRefusal = [](ConstraintAssembly assembly) {
            ProjectJobSchedule s = handSchedule(10, 10, 1);
            try
            {
                levelsWith(std::move(assembly), s);
            }
            catch (const std::logic_error &error)
            {
                return std::string(error.what());
            }
            return std::string();
        };
        auto refusal = [](const std::string &name) {
            return "Constraint '" + name
                   + "' cannot use clip=false with a non-zero offset on a level that counts "
                     "toward the hard score (hard 0 must mean no violation)";
        };
        check(resetRefusal(copyOf("renewableResourceCapacityCost")
                               .offset("renewableResourceCapacityCost", 5.0)
                               .clip("renewableResourceCapacityCost", false))
                      == refusal("renewableResourceCapacityCost")
                  && resetRefusal(copyOf("renewableResourceCapacityCost")
                                      .clip("renewableResourceCapacityCost", false)
                                      .offset("renewableResourceCapacityCost", -1.0))
                         == refusal("renewableResourceCapacityCost"),
              "renewable literal offset +5 / -1 on hard: refused at reset");
        check(resetRefusal(copyOf("nonRenewableResourceCapacityCost")
                               .offset("nonRenewableResourceCapacityCost", 3.5)
                               .clip("nonRenewableResourceCapacityCost", false))
                  == refusal("nonRenewableResourceCapacityCost"),
              "non-renewable literal offset on its default hard level: refused at reset");
        check(resetRefusal(copyOf("makespanCost")
                               .offset("makespanCost", 5.0)
                               .clip("makespanCost", false)
                               .hard("makespanCost"))
                      == refusal("makespanCost")
                  && resetRefusal(copyOf("projectDelayCost")
                                      .hard("projectDelayCost")
                                      .offset("projectDelayCost", 3.0)
                                      .clip("projectDelayCost", false))
                         == refusal("projectDelayCost"),
              "a soft / medium literal pick moved to hard (before or after): refused at reset");
        check(resetRefusal(copyOf("renewableResourceCapacityCost")
                               .offset("renewableResourceCapacityCost", 5.0))
                      .empty()
                  && resetRefusal(copyOf("renewableResourceCapacityCost")
                                      .clip("renewableResourceCapacityCost", false))
                         .empty()
                  && resetRefusal(copyOf("projectDelayCost")
                                      .offset("projectDelayCost", 3.0)
                                      .clip("projectDelayCost", false))
                         .empty(),
              "allowed: a hard threshold offset, hard clip=false with offset 0, and the "
              "literal formula on the into_soft medium");
    }
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        ProjectJobSchedule s = buildRandomInstance(/*projects*/ 2,
                                                   /*standardPerProject*/ 6,
                                                   /*seed*/ 7,
                                                   /*delayRangeSize*/ 12);
        ScoreCalculator::prepare(s);
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::Editor<ProjectJobSchedule> editor(s, calculator);
        ProjectJobSchedule shadow = s;
        std::mt19937_64 rng(321);
        std::vector<int> movable;
        for (int a = 0; a < s.allocationCount(); ++a)
        {
            if (s.jobTypeOf(a) == JobType::STANDARD)
                movable.push_back(a);
        }
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        int mismatches = 0;
        int mediumSteps = 0;
        const int steps = 1500;
        for (int step = 0; step < steps; ++step)
        {
            const int a = movable[rng() % movable.size()];
            if (unit(rng) < 0.75)
            {
                editor.assign(
                    a,
                    unit(rng) < 0.10
                        ? -1
                        : static_cast<int>(rng() % s.value_count()));
            }
            else
            {
                const int b = movable[rng() % movable.size()];
                const int va = s.allocations[a].modeDelay;
                const int vb = s.allocations[b].modeDelay;
                editor.assign(a, vb);
                editor.assign(b, va);
            }
            for (int i = 0; i < s.allocationCount(); ++i)
                shadow.allocations[i].modeDelay = s.allocations[i].modeDelay;
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            fresh.rebuild(shadow);
            const optimaru::Score full = fresh.score();
            const optimaru::Score incremental = editor.score();
            const LevelScores levels = calculator.level_scores();
            const LevelScores fullLevels = fresh.level_scores();
            if (fullLevels.medium != 0.0)
                ++mediumSteps;
            if (!optimaru::scores_agree(incremental, full)
                || levels.hard != fullLevels.hard
                || levels.medium != fullLevels.medium
                || levels.soft != fullLevels.soft)
                ++mismatches;
        }
        check(mismatches == 0,
              std::to_string(steps)
                  + " random moves with repeated, shifted components: "
                    "incremental == fresh");
        check(mediumSteps > 0, "the walk visits medium states");
    }
    {
        ProjectJobSchedule s = buildRandomInstance(/*projects*/ 2,
                                                   /*standardPerProject*/ 5,
                                                   /*seed*/ 11,
                                                   /*delayRangeSize*/ 10);
        Solver::Config config;
        config.time_limit = std::chrono::seconds(2);
        config.seed = 5;
        config.score_checks = optimaru::ScoreChecks::Full;
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        bool corrupted = false;
        try
        {
            Solver solver(config);
            solver.solve(s, calculator);
        }
        catch (const optimaru::ScoreMismatchError &e)
        {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted, "2s FULL_ASSERT solve with repeated, shifted components");
        DecomposedCalculator fresh;
        repeatedAssembly().into(fresh);
        check(!corrupted && s.score == scoreWith(fresh, s),
              "the reported score == a fresh recompute of the same assembly");
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
    verifyRawCostsByHand();
    verifyCatalog();
    verifyMediumLevel();
    verifyMediumIncremental();
    verifyRepeatsAndOffsets();

    if (failures == 0)
    {
        std::printf("All project-job-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d project-job-scheduling plugin check(s) FAILED.\n",
                failures);
    return 1;
}

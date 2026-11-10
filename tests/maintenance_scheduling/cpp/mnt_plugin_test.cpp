// Plugin-assembly (mnt/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedObjective
//       reproduces the monolithic score bit-for-bit on 400 random
//       assignments (unassigned jobs included)
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's Editor protocol (plugin path)
//   (c) partial assemblies: each component in isolation, hand-computed
//   (d) weight / level overrides
//   (e) constraint_totals() explains exactly the reported score
//   (f) a 2-second solve(schedule, calculator) under ScoreChecks::Full
//   (g) the defaultConstraints() builder: named edits == by-hand assembly
//   (h) each component's raw cost, hand-calculated: a positive and a zero
//       case per constraint (raw 0 = no violation)
//   (i) the catalog: info() off the default assembly == the components'
//       kDefaultWeight / kDefaultLevel; the level and knob guards
//   (j) repeated components and the offset / clip knobs: hand-calculated
//       repeats of a per-job and a global rule, threshold and literal
//       offsets by hand (in literal mode every (start date, crew) cell
//       counts, an empty one included; a global shapes its one total), the
//       literal formula refused on hard at reset and kept on soft,
//       incremental == a fresh recompute over a random walk and a
//       ScoreChecks::Full solve with repeated, shifted components
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

#include "mnt/constraints.hpp"
#include "mnt/domain.hpp"
#include "mnt/score_calculator.hpp"
#include "mnt/solver.hpp"
#include "optimaru/model/editor.hpp"

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
                actual.str().c_str(),
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
    calculator.rebuild(s);
    return calculator.score();
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
        const Score monolithic = ScoreCalculator::score(s);
        if (!sameScore(plugin, monolithic))
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
    MaintenanceSchedule s = buildRandomInstance(/*jobCount*/ 24, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::Editor<MaintenanceSchedule> editor(s, calculator);
    MaintenanceSchedule shadow = s;

    std::mt19937_64 rng(123);
    const int jobCount = static_cast<int>(s.jobs.size());
    const int value_count = s.crewDateCount();
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
                                  : static_cast<int>(rng() % value_count);
            editor.assign(job, value);
        }
        else
        {
            const int a = static_cast<int>(rng() % jobCount);
            const int b = static_cast<int>(rng() % jobCount);
            const int va = s.jobs[a].crewDate;
            const int vb = s.jobs[b].crewDate;
            editor.assign(a, vb);
            editor.assign(b, va);
        }
        for (int i = 0; i < jobCount; ++i)
            shadow.jobs[i].crewDate = s.jobs[i].crewDate;
        const Score full = ScoreCalculator::score(shadow);
        const Score incremental = editor.score();
        if (!sameScore(full, incremental))
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
// (c) Single-component assemblies on hand-computed instances.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Single-component assemblies ==\n");

    {  // crewConflictCost alone: two 5-business-day jobs on the same crew
        // and start date overlap on all 5 calendar days they span
        // ([Mon, Sat)).
        MaintenanceSchedule s = makeBase();
        addJob(s, 5);
        addJob(s, 5);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        s.jobs[1].crewDate = s.crewDateOf(0, 0);
        // 5 business days from a Monday -> 5
        const std::int64_t days = s.endDateFor(5, 0) - s.startDates[0];
        DecomposedCalculator calculator;
        calculator.emplace<CrewConflictCost>();
        checkScore(
            "crewConflictCost alone", scoreWith(calculator, s), -days, 0.0);
    }
    {  // Same two jobs on DIFFERENT crews: no conflict at all.
        MaintenanceSchedule s = makeBase();
        addJob(s, 5);
        addJob(s, 5);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        s.jobs[1].crewDate = s.crewDateOf(0, 1);
        DecomposedCalculator calculator;
        calculator.emplace<CrewConflictCost>();
        checkScore("crewConflictCost silent across crews",
                   scoreWith(calculator, s),
                   0,
                   0.0);
    }
    {  // tagConflictCost alone: the same two jobs on different crews,
        // sharing one tag, at 1'000 per overlapping day.
        MaintenanceSchedule s = makeBase();
        addJob(s, 5, {"Downtown"});
        addJob(s, 5, {"Downtown"});
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        s.jobs[1].crewDate = s.crewDateOf(0, 1);
        const std::int64_t days = s.endDateFor(5, 0) - s.startDates[0];
        DecomposedCalculator calculator;
        calculator.emplace<TagConflictCost>();
        checkScore("tagConflictCost alone",
                   scoreWith(calculator, s),
                   0,
                   -1000.0 * static_cast<double>(days));
    }
    {  // minStartDateCost alone: starting 3 days early costs 3.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, /*minStartDate*/ DAY_1 + 3);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);  // starts on DAY_1
        DecomposedCalculator calculator;
        calculator.emplace<MinStartDateCost>();
        checkScore("minStartDateCost alone", scoreWith(calculator, s), -3, 0.0);
    }
    {  // maxEndDateCost alone: a 2-business-day job from DAY_1 ends on
        // DAY_1 + 2, one day past a DAY_1 + 1 deadline.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, kNoDate, /*maxEndDate*/ DAY_1 + 1);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        DecomposedCalculator calculator;
        calculator.emplace<MaxEndDateCost>();
        checkScore("maxEndDateCost alone", scoreWith(calculator, s), -1, 0.0);
    }
    {  // beforeIdealEndDateCost alone: ending on DAY_1 + 2 with an ideal of
        // DAY_1 + 5 is 3 days early, at weight 1.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, kNoDate, kNoDate, /*idealEndDate*/ DAY_1 + 5);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        DecomposedCalculator calculator;
        calculator.emplace<BeforeIdealEndDateCost>();
        checkScore(
            "beforeIdealEndDateCost alone", scoreWith(calculator, s), 0, -3.0);
    }
    {  // afterIdealEndDateCost alone: the same job one day late costs
        // 1'000'000.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, kNoDate, kNoDate, /*idealEndDate*/ DAY_1 + 1);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        DecomposedCalculator calculator;
        calculator.emplace<AfterIdealEndDateCost>();
        checkScore("afterIdealEndDateCost alone",
                   scoreWith(calculator, s),
                   0,
                   -1000000.0);
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
        calculator.emplace<MinStartDateCost>(10.0);
        checkScore("minStartDateCost re-weighted to 10",
                   scoreWith(calculator, s),
                   -30,
                   0.0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<MinStartDateCost>();
        component->level = optimaru::Level::Soft;
        calculator.add(std::move(component));
        checkScore("minStartDateCost demoted to soft",
                   scoreWith(calculator, s),
                   0,
                   -3.0);
    }
    {
        // exponent 2 shapes the raw day count (3) before the weight.
        DecomposedCalculator calculator;
        auto component = std::make_unique<MinStartDateCost>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("minStartDateCost with exponent 2",
                   scoreWith(calculator, s),
                   -9,
                   0.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraint_totals(): the per-constraint breakdown adds up.
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
    for (const optimaru::ConstraintTotal &total : calculator.constraint_totals())
        (total.is_hard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-6,
          "per-constraint totals sum to the reported score");
    check(calculator.constraint_totals().size() == 6,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) ScoreChecks::Full solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    MaintenanceSchedule s = buildRandomInstance(/*jobCount*/ 16, /*seed*/ 11);

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
        check(sameScore(s.score, recomputed),
              "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}


// -------------------------------------------------------------------------
// (g) defaultConstraints(): named edits on the default set.
// -------------------------------------------------------------------------
void verifyAssemblyBuilder()
{
    std::printf("== (g) defaultConstraints() builder ==\n");
    MaintenanceSchedule s = buildRandomInstance(/*jobCount*/ 16, /*seed*/ 21);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(8);
    for (Job &job : s.jobs)
        job.crewDate = static_cast<int>(rng() % s.crewDateCount());

    // By hand: maxEndDateCost demoted to soft, afterIdealEndDateCost at
    // weight 2, beforeIdealEndDateCost dropped, the rest at their defaults.
    DecomposedCalculator byHand;
    {
        byHand.emplace<CrewConflictCost>();
        byHand.emplace<MinStartDateCost>();
        auto maxEnd = std::make_unique<MaxEndDateCost>();
        maxEnd->level = optimaru::Level::Soft;
        byHand.add(std::move(maxEnd));
        byHand.emplace<AfterIdealEndDateCost>(2.0);
        byHand.emplace<TagConflictCost>();
    }
    const Score expected = scoreWith(byHand, s);

    // By name: the same three edits, one expression.
    DecomposedCalculator built;
    defaultConstraints()
        .soft("maxEndDateCost")
        .weight("afterIdealEndDateCost", 2)
        .drop("beforeIdealEndDateCost")
        .into(built);
    check(sameScore(scoreWith(built, s), expected),
          "named edits == the same assembly listed by hand");

    // The default assembly, untouched, still matches the monolith.
    DecomposedCalculator plain;
    defaultConstraints().into(plain);
    check(sameScore(scoreWith(plain, s), ScoreCalculator::score(s)),
          "defaultConstraints() alone == the monolithic score");

    // A typo must not quietly leave the cost function as it was.
    bool threw = false;
    try
    {
        defaultConstraints().weight("tagConflictCosts", 4);  // trailing s
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    check(threw, "an unknown name is refused (std::invalid_argument)");

    // only() isolates a subset: just the crew rule, nothing else scored.
    DecomposedCalculator isolated;
    defaultConstraints().only({"crewConflictCost"}).into(isolated);
    DecomposedCalculator crewOnly;
    crewOnly.emplace<CrewConflictCost>();
    check(sameScore(scoreWith(isolated, s), scoreWith(crewOnly, s)),
          "only() scores exactly the named constraint");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) Each component's raw cost on hand-built instances: one violating
// (positive) case and one clean (zero) case per constraint, read through
// constraint_costs() — the raw numbers before weight, exponent and level.
// Per-job rules report one cost per (start date, crew) cell; the two
// pairwise rules are global and report a single total.
// -------------------------------------------------------------------------
DecomposedCalculator::ConstraintCosts rawCosts(MaintenanceSchedule &schedule,
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
            return entry;
    }
    throw std::logic_error("no constraint named " + name);
}

// The per-cell list of a per-job rule with `value` in cell `crewDate` and 0
// everywhere else.
std::vector<double> oneCell(const MaintenanceSchedule &schedule,
                            int crewDate,
                            double value)
{
    std::vector<double> costs(
        static_cast<std::size_t>(schedule.crewDateCount()), 0.0);
    costs[static_cast<std::size_t>(crewDate)] = value;
    return costs;
}

void verifyRawCostsByHand()
{
    std::printf("== (h) Raw costs by hand, positive and zero ==\n");
    {
        // Two 5-business-day jobs, both on crew 0 from DAY_1 (a Monday):
        // [Mon, Sat) = [DAY_1, DAY_1 + 5) each (the exclusive end may land
        // on a Saturday), so they overlap on 5 calendar days.
        MaintenanceSchedule s = makeBase();
        addJob(s, 5);
        addJob(s, 5);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        s.jobs[1].crewDate = s.crewDateOf(0, 0);
        const auto entry = rawCosts(s, "crewConflictCost");
        check(entry.is_global && entry.unit_costs == std::vector<double>{5.0},
              "crewConflictCost: [5] (same crew, 5 shared days)");
    }
    {
        // The same two jobs on different crews.
        MaintenanceSchedule s = makeBase();
        addJob(s, 5);
        addJob(s, 5);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        s.jobs[1].crewDate = s.crewDateOf(0, 1);
        check(rawCosts(s, "crewConflictCost").unit_costs
                  == std::vector<double>{0.0},
              "crewConflictCost: [0] on different crews");
    }
    {
        // Two 1-day jobs on different crews on DAY_1, sharing BOTH tags:
        // 1 shared day x 2 shared tags = 2.
        MaintenanceSchedule s = makeBase();
        addJob(s, 1, {"Downtown", "Park"});
        addJob(s, 1, {"Park", "Downtown", "Airport"});
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        s.jobs[1].crewDate = s.crewDateOf(0, 1);
        const auto entry = rawCosts(s, "tagConflictCost");
        check(entry.is_global && entry.unit_costs == std::vector<double>{2.0},
              "tagConflictCost: [2] (2 shared tags x 1 day)");
    }
    {
        // Shared tag, but the second job runs the next day: no overlap.
        MaintenanceSchedule s = makeBase();
        addJob(s, 1, {"Downtown"});
        addJob(s, 1, {"Downtown"});
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        s.jobs[1].crewDate = s.crewDateOf(1, 1);
        check(rawCosts(s, "tagConflictCost").unit_costs
                  == std::vector<double>{0.0},
              "tagConflictCost: [0] on consecutive days");
    }
    {
        // Starts on DAY_1, may start on DAY_1 + 3 at the earliest.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, /*minStartDate*/ DAY_1 + 3);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 1);
        check(rawCosts(s, "minStartDateCost").unit_costs
                  == oneCell(s, s.crewDateOf(0, 1), 3.0),
              "minStartDateCost: 3 in the job's cell, 0 elsewhere");
    }
    {
        // Starts exactly on its minStartDate.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, /*minStartDate*/ DAY_1);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        check(rawCosts(s, "minStartDateCost").unit_costs
                  == oneCell(s, s.crewDateOf(0, 0), 0.0),
              "minStartDateCost: all 0 when starting on minStartDate");
    }
    {
        // 2 business days from DAY_1 ends (exclusive) on DAY_1 + 2, one day
        // past a DAY_1 + 1 maxEndDate.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, kNoDate, /*maxEndDate*/ DAY_1 + 1);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        check(rawCosts(s, "maxEndDateCost").unit_costs
                  == oneCell(s, s.crewDateOf(0, 0), 1.0),
              "maxEndDateCost: 1 in the job's cell, 0 elsewhere");
    }
    {
        // The same job with maxEndDate DAY_1 + 2: ends exactly in time.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, kNoDate, /*maxEndDate*/ DAY_1 + 2);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        check(rawCosts(s, "maxEndDateCost").unit_costs
                  == oneCell(s, s.crewDateOf(0, 0), 0.0),
              "maxEndDateCost: all 0 when ending on maxEndDate");
    }
    {
        // Ends on DAY_1 + 2, ideally on DAY_1 + 5: 3 days early.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, kNoDate, kNoDate, /*idealEndDate*/ DAY_1 + 5);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        check(rawCosts(s, "beforeIdealEndDateCost").unit_costs
                  == oneCell(s, s.crewDateOf(0, 0), 3.0),
              "beforeIdealEndDateCost: 3 in the job's cell, 0 elsewhere");
        check(rawCosts(s, "afterIdealEndDateCost").unit_costs
                  == oneCell(s, s.crewDateOf(0, 0), 0.0),
              "afterIdealEndDateCost: all 0 when ending early");
    }
    {
        // Ends on DAY_1 + 2, ideally on DAY_1 + 1: 1 day late.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, kNoDate, kNoDate, /*idealEndDate*/ DAY_1 + 1);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        check(rawCosts(s, "afterIdealEndDateCost").unit_costs
                  == oneCell(s, s.crewDateOf(0, 0), 1.0),
              "afterIdealEndDateCost: 1 in the job's cell, 0 elsewhere");
        check(rawCosts(s, "beforeIdealEndDateCost").unit_costs
                  == oneCell(s, s.crewDateOf(0, 0), 0.0),
              "beforeIdealEndDateCost: all 0 when ending late");
    }
    {
        // Ends exactly on its idealEndDate: neither side fires.
        MaintenanceSchedule s = makeBase();
        addJob(s, 2, {}, kNoDate, kNoDate, /*idealEndDate*/ DAY_1 + 2);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(0, 0);
        const std::vector<double> zero = oneCell(s, s.crewDateOf(0, 0), 0.0);
        check(rawCosts(s, "beforeIdealEndDateCost").unit_costs == zero
                  && rawCosts(s, "afterIdealEndDateCost").unit_costs == zero,
              "before/afterIdealEndDateCost: all 0 when ending on the ideal "
              "date");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The catalog read off the default assembly is exactly the components'
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
           double weight,
           bool is_global)
{
    return row.name == name && row.default_level == Component::kDefaultLevel
           && row.default_weight == Component::kDefaultWeight
           && row.default_level == level && row.default_weight == weight
           && row.level_flippable && !row.reward && row.is_global == is_global
           && row.default_enabled;
}

void verifyCatalog()
{
    std::printf("== (i) Catalog and guards ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    // Unit constraints first, then the globals: the list_constraints()
    // order.
    check(rows.size() == 6
              && rowIs<MinStartDateCost>(
                  rows[0], "minStartDateCost", Level::Hard, 1.0, false)
              && rowIs<MaxEndDateCost>(
                  rows[1], "maxEndDateCost", Level::Hard, 1.0, false)
              && rowIs<BeforeIdealEndDateCost>(
                  rows[2], "beforeIdealEndDateCost", Level::Soft, 1.0, false)
              && rowIs<AfterIdealEndDateCost>(rows[3],
                                              "afterIdealEndDateCost",
                                              Level::Soft,
                                              1000000.0,
                                              false)
              && rowIs<CrewConflictCost>(
                  rows[4], "crewConflictCost", Level::Hard, 1.0, true)
              && rowIs<TagConflictCost>(
                  rows[5], "tagConflictCost", Level::Soft, 1000.0, true),
          "info(): crew/minStart/maxEnd hard 1, beforeIdeal soft 1, "
          "afterIdeal soft 1e6, tag soft 1e3; all flippable, no reward, all "
          "default-enabled; crew and tag global");
    check(!throwsInvalid([] { defaultConstraints().soft("crewConflictCost"); })
              && !throwsInvalid(
                  [] { defaultConstraints().hard("tagConflictCost"); })
              && !throwsInvalid(
                  [] { defaultConstraints().hard("afterIdealEndDateCost"); }),
          "every level flips both ways");
    check(throwsInvalid(
              [] { defaultConstraints().weight("tagConflictCost", -2.0); }),
          "a negative weight is refused");
    check(throwsInvalid(
              [] { defaultConstraints().exponent("maxEndDateCost", 0.0); }),
          "a non-positive exponent is refused");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with add_instances(), as the Python bindings
// build it from a pick list.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string &name)
{
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// Two 5-business-day jobs from DAY_1 on crew 0, both tagged Downtown: they
// overlap on all 5 calendar days they span ([Mon, Sat)), so crewConflictCost
// and tagConflictCost both total 5. Job 0 may start on DAY_1 + 3 only, so
// minStartDateCost is 3 on cell (date 0, crew 0) and 0 on the other 79 cells
// (40 business days x 2 crews).
MaintenanceSchedule repeatInstance()
{
    MaintenanceSchedule s = makeBase();
    addJob(s, 5, {"Downtown"}, /*minStartDate*/ DAY_1 + 3);
    addJob(s, 5, {"Downtown"});
    ScoreCalculator::prepare(s);
    s.jobs[0].crewDate = s.crewDateOf(0, 0);
    s.jobs[1].crewDate = s.crewDateOf(0, 0);
    return s;
}

// The defaults, two of them shifted (crew conflict by a threshold: hard
// refuses the literal formula with an offset), plus repeats of a per-job
// delta rule (literal on soft, fractional weight), a global member-change
// rule (threshold, squared; and literal on soft) and a soft per-job rule
// (negative offset), and two repeats on the other level than their default:
// max end demoted to soft (literal, fractional weight) and tag conflict
// promoted to hard (threshold).
ConstraintAssembly repeatedAssembly()
{
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("afterIdealEndDateCost", 2.0);
    assembly.offset("crewConflictCost", 1.0);
    assembly.add_instances(std::move(copyOf("crewConflictCost")
                                        .soft("crewConflictCost")
                                        .offset("crewConflictCost", 1.0)
                                        .clip("crewConflictCost", false)));
    assembly.add_instances(std::move(copyOf("minStartDateCost")
                                        .soft("minStartDateCost")
                                        .weight("minStartDateCost", 2.0)
                                        .offset("minStartDateCost", 1.0)
                                        .clip("minStartDateCost", false)));
    assembly.add_instances(std::move(copyOf("tagConflictCost")
                                        .weight("tagConflictCost", 0.25)
                                        .exponent("tagConflictCost", 2.0)
                                        .offset("tagConflictCost", 3.0)));
    assembly.add_instances(std::move(copyOf("beforeIdealEndDateCost")
                                        .weight("beforeIdealEndDateCost", 0.5)
                                        .offset("beforeIdealEndDateCost", -2.0)));
    assembly.add_instances(std::move(copyOf("crewConflictCost")));
    assembly.add_instances(std::move(copyOf("maxEndDateCost")
                                        .soft("maxEndDateCost")
                                        .weight("maxEndDateCost", 1.5)
                                        .offset("maxEndDateCost", 1.0)
                                        .clip("maxEndDateCost", false)));
    assembly.add_instances(std::move(copyOf("tagConflictCost")
                                        .hard("tagConflictCost")
                                        .weight("tagConflictCost", 2.0)
                                        .offset("tagConflictCost", 4.0)));
    return assembly;
}

void verifyRepeatsAndOffsets()
{
    std::printf("== (j) Repeated components, offset / clip ==\n");
    {
        MaintenanceSchedule s = repeatInstance();
        check(s.crewDateCount() == 80, "80 (start date, crew) cells");
        ConstraintAssembly assembly = copyOf("crewConflictCost");
        assembly.add_instances(std::move(copyOf("crewConflictCost")
                                            .weight("crewConflictCost", 2.0)
                                            .exponent("crewConflictCost", 2.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        checkScore("crew conflict + 2 x crew conflict^2: -(5 + 50)",
                   scoreWith(calculator, s),
                   -55,
                   0.0);
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].unit_costs == entries[1].unit_costs
                  && entries[0].unit_costs.size() == 1
                  && entries[1].weight == 2.0 && entries[1].exponent == 2.0
                  && entries[1].default_weight == 1.0,
              "two global instances, the same total, each with its own knobs");
    }
    {
        MaintenanceSchedule s = repeatInstance();
        ConstraintAssembly assembly = copyOf("minStartDateCost");
        assembly.add_instances(std::move(copyOf("minStartDateCost")
                                            .weight("minStartDateCost", 3.0)
                                            .exponent("minStartDateCost", 2.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        checkScore("min start + 3 x min start^2 per cell: -(3 + 27)",
                   scoreWith(calculator, s),
                   -30,
                   0.0);
    }
    {
        // Each instance on its own level: the soft default tag conflict
        // (1000 x 5) next to a hard repeat, 2 x (5 - 1) beyond its offset.
        MaintenanceSchedule s = repeatInstance();
        ConstraintAssembly assembly = copyOf("tagConflictCost");
        assembly.add_instances(std::move(copyOf("tagConflictCost")
                                            .hard("tagConflictCost")
                                            .weight("tagConflictCost", 2.0)
                                            .offset("tagConflictCost", 1.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        checkScore("soft tag conflict + a hard repeat beyond 1: -8hard",
                   scoreWith(calculator, s),
                   -8,
                   -5000.0);
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].level == optimaru::Level::Soft
                  && entries[1].level == optimaru::Level::Hard
                  && entries[1].default_level == optimaru::Level::Soft,
              "the repeat reports its own level, the default stays soft");
    }
    {
        // A hard per-cell rule with a soft literal repeat: 0.5 x (3 - 1) on
        // the early cell, 0.5 x (0 - 1) on each of the 79 others.
        MaintenanceSchedule s = repeatInstance();
        ConstraintAssembly assembly = copyOf("minStartDateCost");
        assembly.add_instances(std::move(copyOf("minStartDateCost")
                                            .soft("minStartDateCost")
                                            .weight("minStartDateCost", 0.5)
                                            .offset("minStartDateCost", 1.0)
                                            .clip("minStartDateCost", false)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        checkScore("hard min start + a soft literal repeat: -(1 - 39.5)soft",
                   scoreWith(calculator, s),
                   -3,
                   38.5);
    }
    {
        MaintenanceSchedule s = repeatInstance();
        DecomposedCalculator threshold;
        copyOf("minStartDateCost")
            .offset("minStartDateCost", 1.0)
            .into(threshold);
        checkScore("threshold offset 1: max(0, 3 - 1)",
                   scoreWith(threshold, s),
                   -2,
                   0.0);
        // Literal, on soft (hard refuses it with an offset): the 79 empty
        // cells are each rewarded 1.
        DecomposedCalculator literal;
        copyOf("minStartDateCost")
            .soft("minStartDateCost")
            .offset("minStartDateCost", 1.0)
            .clip("minStartDateCost", false)
            .into(literal);
        checkScore("soft literal offset 1: (3 - 1) + 79 x (0 - 1), every cell",
                   scoreWith(literal, s),
                   0,
                   77.0);
        DecomposedCalculator negative;
        copyOf("minStartDateCost")
            .offset("minStartDateCost", -2.0)
            .into(negative);
        checkScore("threshold offset -2 adds to the non-zero cell only",
                   scoreWith(negative, s),
                   -5,
                   0.0);
        DecomposedCalculator negativeLiteral;
        copyOf("minStartDateCost")
            .soft("minStartDateCost")
            .offset("minStartDateCost", -2.0)
            .clip("minStartDateCost", false)
            .into(negativeLiteral);
        checkScore("soft literal offset -2 adds to every cell: 5 + 79 x 2",
                   scoreWith(negativeLiteral, s),
                   0,
                   -163.0);
        // A hard threshold offset rounds per cell: (3 - 0.5) x 3 = 7.5 -> 8,
        // and the empty cells stay 0.
        DecomposedCalculator hard;
        copyOf("minStartDateCost")
            .weight("minStartDateCost", 3.0)
            .offset("minStartDateCost", 0.5)
            .into(hard);
        checkScore("hard threshold offset 0.5 at weight 3, rounded per cell",
                   scoreWith(hard, s),
                   -8,
                   0.0);
        // The same literal pick on soft is not rounded: 7.5 - 79 x 1.5.
        DecomposedCalculator soft;
        copyOf("minStartDateCost")
            .soft("minStartDateCost")
            .weight("minStartDateCost", 3.0)
            .offset("minStartDateCost", 0.5)
            .clip("minStartDateCost", false)
            .into(soft);
        checkScore("soft literal offset 0.5 at weight 3: -(7.5 - 118.5)",
                   scoreWith(soft, s),
                   0,
                   111.0);
        // A global shapes its one total.
        DecomposedCalculator tagThreshold;
        copyOf("tagConflictCost").offset("tagConflictCost", 2.0).into(tagThreshold);
        checkScore("tag threshold offset 2: 1000 x (5 - 2)",
                   scoreWith(tagThreshold, s),
                   0,
                   -3000.0);
        DecomposedCalculator tagLiteral;
        copyOf("tagConflictCost")
            .offset("tagConflictCost", 8.0)
            .clip("tagConflictCost", false)
            .into(tagLiteral);
        checkScore("tag literal offset 8: 1000 x (5 - 8), a reward",
                   scoreWith(tagLiteral, s),
                   0,
                   3000.0);
    }
    {
        // On hard the literal formula with an offset is refused at reset,
        // whichever sign and setter order, for a hard default (a per-cell
        // and a global rule) and for a soft rule moved to hard: +1 would
        // reward the 79 empty cells (+77 hard, a feasible-looking score for
        // a job that starts too early), -1 would charge 80 cells that hold
        // no violation.
        auto refusal = [](const std::string &name) {
            return "Constraint '" + name
                   + "' cannot use clip=false with a non-zero offset on a level "
                     "that counts toward the hard score (hard 0 must mean no "
                     "violation)";
        };
        auto resetRefusal = [](ConstraintAssembly assembly) {
            MaintenanceSchedule s = repeatInstance();
            DecomposedCalculator calculator;
            assembly.into(calculator);
            try
            {
                scoreWith(calculator, s);
            }
            catch (const std::logic_error &error)
            {
                return std::string(error.what());
            }
            return std::string();
        };
        check(resetRefusal(copyOf("minStartDateCost")
                               .offset("minStartDateCost", 1.0)
                               .clip("minStartDateCost", false))
                      == refusal("minStartDateCost")
                  && resetRefusal(copyOf("minStartDateCost")
                                      .clip("minStartDateCost", false)
                                      .offset("minStartDateCost", -1.0))
                         == refusal("minStartDateCost"),
              "min start literal offset +-1 on hard: refused at reset");
        check(resetRefusal(copyOf("crewConflictCost")
                               .offset("crewConflictCost", 1.0)
                               .clip("crewConflictCost", false))
                  == refusal("crewConflictCost"),
              "crew conflict (a global) literal offset on hard: refused at reset");
        check(resetRefusal(copyOf("tagConflictCost")
                               .offset("tagConflictCost", 8.0)
                               .clip("tagConflictCost", false)
                               .hard("tagConflictCost"))
                  == refusal("tagConflictCost"),
              "tag literal offset moved to hard afterwards: refused at reset");
        ConstraintAssembly repeated = copyOf("minStartDateCost");
        repeated.add_instances(std::move(copyOf("minStartDateCost")
                                            .offset("minStartDateCost", 1.0)
                                            .clip("minStartDateCost", false)));
        check(resetRefusal(std::move(repeated)) == refusal("minStartDateCost"),
              "a hard literal repeat next to the default: refused at reset");
        check(resetRefusal(copyOf("minStartDateCost")
                               .offset("minStartDateCost", 1.0))
                      .empty()
                  && resetRefusal(copyOf("minStartDateCost")
                                      .clip("minStartDateCost", false))
                         .empty()
                  && resetRefusal(copyOf("minStartDateCost")
                                      .offset("minStartDateCost", 1.0)
                                      .clip("minStartDateCost", false)
                                      .soft("minStartDateCost"))
                         .empty(),
              "min start: a threshold offset or clip=false with offset 0 on "
              "hard, and the literal offset on soft, are allowed");
    }
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        MaintenanceSchedule s = buildRandomInstance(/*jobCount*/ 24, /*seed*/ 7);
        ScoreCalculator::prepare(s);
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::Editor<MaintenanceSchedule> editor(s, calculator);
        std::mt19937_64 rng(321);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const int jobCount = static_cast<int>(s.jobs.size());
        const int value_count = s.crewDateCount();
        int mismatches = 0;
        double drift = 0.0;
        const int steps = 3000;
        for (int step = 0; step < steps; ++step)
        {
            if (unit(rng) < 0.75)
            {
                const int job = static_cast<int>(rng() % jobCount);
                const int value = unit(rng) < 0.10
                                      ? -1
                                      : static_cast<int>(rng() % value_count);
                editor.assign(job, value);
            }
            else
            {
                const int a = static_cast<int>(rng() % jobCount);
                const int b = static_cast<int>(rng() % jobCount);
                const int va = s.jobs[a].crewDate;
                const int vb = s.jobs[b].crewDate;
                editor.assign(a, vb);
                editor.assign(b, va);
            }
            MaintenanceSchedule shadow = s;
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            const Score full = scoreWith(fresh, shadow);
            const Score incremental = editor.score();
            drift = std::max(drift, std::fabs(full.soft - incremental.soft));
            // The engine's ScoreChecks::Full rule (scores_agree): hard exact, soft
            // within 1e-6 (the fractional weights accumulate a few ulps).
            if (!optimaru::scores_agree(incremental, full))
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
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0,
              std::to_string(steps)
                  + " random moves with repeated, shifted components on both "
                    "levels: incremental == fresh");
    }
    {
        MaintenanceSchedule s = buildRandomInstance(/*jobCount*/ 16, /*seed*/ 11);
        Solver::Config config;
        config.time_limit = std::chrono::seconds(2);
        config.seed = 5;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruinRecreateUnimprovedStepTrigger = 200;
        config.ruinRecreatePolishSteps = 100;
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
        check(!corrupted && optimaru::scores_agree(s.score, scoreWith(fresh, s)),
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
    verifyAssemblyBuilder();
    verifyRawCostsByHand();
    verifyCatalog();
    verifyRepeatsAndOffsets();

    if (failures == 0)
    {
        std::printf("All maintenance-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d maintenance-scheduling plugin check(s) FAILED.\n",
                failures);
    return 1;
}

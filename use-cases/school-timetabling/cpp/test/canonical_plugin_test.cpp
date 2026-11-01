// Plugin-assembly (st/canonical_constraints.hpp) tests, engine test/ style:
// check() + a non-zero exit on any failure.
//   (a) the full default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (including unassigned lessons)
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: removing one component removes exactly that
//       term, on hand-computed instances
//   (d) weight / level overrides: teacherMaxPeriodsCost demoted to soft,
//       teacherGapCost weight doubled
//   (e) a 2-second solve(table, calculator) under FULL_ASSERT (custom move
//       operators included)
//   (f) unassignedLessonsCost demoted to soft turns "leave it unassigned"
//       into a move the search can make (partial timetabling), and leaves it
//       alone while it is hard
//   (g) the plugin-path solve reaches hard 0 on the CP-SAT-sample-scale
//       school within a 20-second budget
//   (h) raw costs by hand: one positive and one zero case per constraint,
//       read through constraintCosts()
//   (i) the catalog: defaultConstraints().info() equals every component's
//       kDefaultWeight / kDefaultLevel / flippability, in constraintOrder();
//       the structural constraints refuse soft() and accept hard()
//   (j) repeated components and the offset / clip knobs: a hand-calculated
//       repeat, threshold and literal offsets by hand on a global, a
//       per-slot and a hard constraint (literal mode charges every slot),
//       incremental == a fresh recompute over a random walk and a
//       FULL_ASSERT solve with repeated, shifted components — the repeated
//       teacher-mask instances sharing the assembly's one TeacherMaskHub
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "st/canonical_constraints.hpp"
#include "st/canonical_domain.hpp"
#include "st/canonical_incremental_calculator.hpp"
#include "st/canonical_score_calculator.hpp"
#include "st/canonical_solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace st::canon;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

void checkCount(const std::string &label,
                std::int64_t got,
                std::int64_t expected)
{
    const bool ok = got == expected;
    std::printf("  [%s] %-52s got=%lld expected=%lld\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                static_cast<long long>(got),
                static_cast<long long>(expected));
    if (!ok)
        ++failures;
}

const ObjectiveWeights kZeroWeights{0, 0, 0, 0, 0, 0};

// Base grid: slots enumerated day-major from periodsPerDay.
Timetable makeGrid(const std::vector<int> &periodsPerDay)
{
    Timetable t;
    for (int d = 0; d < static_cast<int>(periodsPerDay.size()); ++d)
    {
        for (int p = 1; p <= periodsPerDay[d]; ++p)
        {
            t.dayOfSlot.push_back(d);
            t.periodOfSlot.push_back(p);
        }
    }
    return t;
}

// Slot index for (day, 1-based period) in a makeGrid layout (usable before
// prepare()).
int slotIdx(const std::vector<int> &periodsPerDay, int day, int period)
{
    int s = 0;
    for (int d = 0; d < day; ++d)
        s += periodsPerDay[d];
    return s + period - 1;
}

int addLesson(Timetable &t,
              int classId,
              int courseKey,
              std::vector<int> teachers,
              int slot = -1)
{
    Lesson lesson;
    lesson.id = "L" + std::to_string(t.lessons.size());
    lesson.classGroupId = classId;
    lesson.courseKey = courseKey;
    lesson.teachers = std::move(teachers);
    lesson.slot = slot;
    t.lessons.push_back(std::move(lesson));
    return static_cast<int>(t.lessons.size()) - 1;
}

// The default assembly at the given soft weights: what the monolithic path
// scores with these ObjectiveWeights (the model carries no weights).
ConstraintAssembly weightedDefaults(const ObjectiveWeights &w)
{
    return defaultConstraints()
        .weight("teacherGapCost", w.teacherGapCost)
        .weight("teacherWorkdayCost", w.teacherWorkdayCost)
        .weight("teacherFreeDayCost", w.teacherFreeDayCost)
        .weight("classGapCost", w.classGapCost)
        .weight("avoidedPeriodCost", w.avoidedPeriodCost)
        .weight("nonPreferredPeriodCost", w.nonPreferredPeriodCost);
}

// The full set minus one component (by registry name; empty = all
// seventeen), soft weights from `w`. Built component by component, without
// a shared TeacherMaskHub, so the standalone (private-hub) path of the mask
// family stays covered next to defaultConstraints()'s shared one.
void assembleExcept(DecomposedCalculator &calc,
                    const ObjectiveWeights &w,
                    const std::string &skip)
{
    auto keep = [&skip](const char *name) { return skip != name; };
    if (keep("unassignedLessonsCost"))
        calc.emplace<UnassignedLessonsCost>();
    if (keep("classConflictCost"))
        calc.emplace<ClassConflictCost>();
    if (keep("teacherConflictCost"))
        calc.emplace<TeacherConflictCost>();
    if (keep("disallowedSlotCost"))
        calc.emplace<DisallowedSlotCost>();
    if (keep("roomCapacityCost"))
        calc.emplace<RoomCapacityCost>();
    if (keep("courseMaxPeriodsPerDayCost"))
        calc.emplace<CourseMaxPeriodsPerDayCost>();
    if (keep("blockAdjacencyCost"))
        calc.emplace<BlockAdjacencyCost>();
    if (keep("teacherMaxPeriodsCost"))
        calc.emplace<TeacherMaxPeriodsCost>();
    if (keep("teacherMaxConsecutivePeriodsCost"))
        calc.emplace<TeacherMaxConsecutivePeriodsCost>();
    if (keep("teacherGapCost"))
        calc.emplace<TeacherGapCost>(w.teacherGapCost);
    if (keep("teacherWorkdayCost"))
        calc.emplace<TeacherWorkdayCost>(w.teacherWorkdayCost);
    if (keep("teacherFreeDayCost"))
        calc.emplace<TeacherFreeDayCost>(w.teacherFreeDayCost);
    if (keep("classGapCost"))
        calc.emplace<ClassGapCost>(w.classGapCost);
    if (keep("avoidedPeriodCost"))
        calc.emplace<AvoidedPeriodCost>(w.avoidedPeriodCost);
    if (keep("nonPreferredPeriodCost"))
        calc.emplace<NonPreferredPeriodCost>(w.nonPreferredPeriodCost);
    if (keep("fixedSlotsCost"))
        calc.emplace<FixedSlotsCost>();
    if (keep("parallelGroupsCost"))
        calc.emplace<ParallelGroupsCost>();
}

// -------------------------------------------------------------------------
// Random feature-complete instance (same shape as canonical_test.cpp): team
// teaching, blocks, room demand, period masks, tight teacher limits, all
// soft weights on.
// -------------------------------------------------------------------------
const std::vector<int> kGridRandom{5, 5, 5, 4};

Timetable buildRandomInstance(int courseCount, std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    Timetable t = makeGrid(kGridRandom);
    const int slotCount = t.slotCount();
    const int classes = 4;
    const int teachers = 6;
    t.teacherCount = teachers;
    t.classCount = classes;
    t.roomCapacityByType = {1, 2};
    t.teacherMaxPerDay.assign(teachers, 4);
    t.teacherMaxPerWeek.assign(teachers, 9);
    t.teacherMaxConsecutive.assign(teachers, 2);
    t.teacherPreferredFreeDays.assign(teachers, 0);
    t.teacherPreferredFreeDays[0] = 1 << 3;
    t.teacherPreferredFreeDays[3] = 1 << 0;

    int nextBlock = 0;
    for (int c = 0; c < courseCount; ++c)
    {
        t.courseDailyLimit.push_back(1 + static_cast<int>(rng() % 2));
        t.courseAvoidedPeriodMask.push_back(rng() % 3 == 0 ? 0x0001 : 0);
        t.coursePreferredPeriodMask.push_back(rng() % 3 == 0 ? 0x000E : 0);

        const int classId = static_cast<int>(rng() % classes);
        std::vector<int> teach{static_cast<int>(rng() % teachers)};
        // Guarantee team teaching and block coverage, plus a random sprinkle.
        if (c == 2 || rng() % 4 == 0)  // team teaching / merged lesson
        {
            teach.push_back(
                (teach[0] + 1 + static_cast<int>(rng() % (teachers - 1)))
                % teachers);
        }
        const int periods = 2 + static_cast<int>(rng() % 3);
        const int roomType = rng() % 3 == 0 ? static_cast<int>(rng() % 2) : -1;
        const int block = (c < 2 || rng() % 4 == 0) ? nextBlock++ : -1;
        if (block >= 0)
            t.courseDailyLimit.back() = 2;

        for (int i = 0; i < periods; ++i)
        {
            const int lesson = addLesson(t, classId, c, teach);
            if (roomType >= 0)
            {
                t.lessons[lesson].roomTypeDemand.assign(2, 0);
                t.lessons[lesson].roomTypeDemand[roomType] = 1;
            }
            if (block >= 0 && i < 2)
                t.lessons[lesson].blockGroup = block;
            if (rng() % 3 == 0)  // merged-lesson style multi-unit
                                 // avoidedPeriodCost / nonPreferredPeriodCost
            {
                t.lessons[lesson].preferenceUnits
                    = {{static_cast<std::uint16_t>(rng() % 4),
                        static_cast<std::uint16_t>(rng() % 8)},
                       {static_cast<std::uint16_t>(rng() % 4),
                        static_cast<std::uint16_t>(rng() % 8)}};
            }
            if (rng() % 2 == 0)  // per-lesson allowed table with random holes
            {
                std::vector<char> allowed(slotCount, 1);
                for (int s = 0; s < slotCount; ++s)
                {
                    if (rng() % 8 == 0)
                        allowed[s] = 0;
                }
                t.lessons[lesson].allowedSlot = std::move(allowed);
            }
        }
    }
    return t;
}

// -------------------------------------------------------------------------
// (a) Full default assembly == monolithic full recompute, bit for bit, on
// 500 random assignments (10% of the slots unassigned).
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Full assembly == monolithic, 500 random rounds ==\n");
    Timetable t = buildRandomInstance(/*courseCount*/ 16, /*seed*/ 7);
    ScoreCalculator::prepare(t);
    std::printf("  instance: %d lessons, %d slots, %d block groups\n",
                static_cast<int>(t.lessons.size()),
                t.slotCount(),
                t.blockGroupCount);

    DecomposedCalculator calc;
    addCanonicalConstraints(calc);

    std::mt19937_64 rng(2024);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const int lessonCount = static_cast<int>(t.lessons.size());
    const int slotCount = t.slotCount();

    int mismatches = 0;
    const int rounds = 500;
    for (int round = 0; round < rounds; ++round)
    {
        for (int i = 0; i < lessonCount; ++i)
        {
            t.lessons[i].slot
                = unit(rng) < 0.10 ? -1 : static_cast<int>(rng() % slotCount);
        }
        const Score full
            = ScoreCalculator::computeBreakdown(t).toScore(ObjectiveWeights{});
        calc.resetWorkingSolution(t);
        const Score assembled = calc.calculateScore();
        if (assembled.hard != full.hard || assembled.soft != full.soft)
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @round %d: assembly=%s full=%s\n",
                            round,
                            assembled.toString().c_str(),
                            full.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(rounds)
              + " random assignments, assembly == monolithic bit-for-bit");

    // The same at non-default (and non-integer) soft weights: the assembly
    // re-weighted by name scores like the monolithic calculators given the
    // same ObjectiveWeights (full recompute and the incremental calculator).
    const ObjectiveWeights custom{7.0, 2.5, 11.0, 4.0, 9.0, 3.0};
    DecomposedCalculator weighted;
    weightedDefaults(custom).into(weighted);
    CanonicalIncrementalScoreCalculator monolithic(custom);
    int weightedMismatches = 0;
    for (int round = 0; round < 100; ++round)
    {
        for (int i = 0; i < lessonCount; ++i)
        {
            t.lessons[i].slot
                = unit(rng) < 0.10 ? -1 : static_cast<int>(rng() % slotCount);
        }
        const Score full = ScoreCalculator::computeBreakdown(t).toScore(custom);
        weighted.resetWorkingSolution(t);
        monolithic.resetWorkingSolution(t);
        const Score assembled = weighted.calculateScore();
        const Score incremental = monolithic.calculateScore();
        if (assembled.hard != full.hard || assembled.soft != full.soft
            || incremental.hard != full.hard || incremental.soft != full.soft)
        {
            ++weightedMismatches;
        }
    }
    check(weightedMismatches == 0,
          "100 random assignments at custom soft weights: assembly == "
          "monolithic (full and incremental)");

    // The per-constraint breakdown must add up to the score it explains.
    double hardTotal = 0.0;
    double softTotal = 0.0;
    for (const optimaru::ConstraintTotal &total : calc.constraintTotals())
        (total.level == optimaru::Level::Hard ? hardTotal : softTotal)
            += total.cost;
    const Score score = calc.calculateScore();
    check(hardTotal == static_cast<double>(-score.hard)
              && softTotal == -score.soft,
          "constraintTotals() sums to the assembled score");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a ScoreDirector-driven
// random walk of change and swap moves (including unassignments) — the
// before/set/after hook contract, on the plugin path.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full, 3000 director moves ==\n");
    Timetable t = buildRandomInstance(/*courseCount*/ 16, /*seed*/ 7);
    ScoreCalculator::prepare(t);

    DecomposedCalculator calc;
    addCanonicalConstraints(calc);
    optimaru::ScoreDirector<Timetable> director(t, calc);
    Timetable shadow = t;

    std::mt19937_64 rng(123);
    const int lessonCount = static_cast<int>(t.lessons.size());
    const int valueCount = t.slotCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            // ChangeMove; 10% of the time to unassigned (-1), like the
            // construction heuristic's starting state.
            const int lesson = static_cast<int>(rng() % lessonCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(lesson, value);
        }
        else
        {
            // SwapMove, bracketed like the engine does it.
            const int a = static_cast<int>(rng() % lessonCount);
            const int b = static_cast<int>(rng() % lessonCount);
            const int va = t.lessons[a].slot;
            const int vb = t.lessons[b].slot;
            director.beginVariableChanges();
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
            director.endVariableChanges();
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < lessonCount; ++i)
            shadow.lessons[i].slot = t.lessons[i].slot;
        const Score full = ScoreCalculator::computeBreakdown(shadow).toScore(
            ObjectiveWeights{});
        const Score incremental = director.calculateScore();
        if (full.hard != incremental.hard || full.soft != incremental.soft)
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
// (c) Partial assemblies on hand-computed instances: dropping one component
// removes exactly that term.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Partial assemblies, hand-computed ==\n");
    const std::vector<int> grid{5, 5};

    // Case 1 (hard): one teacherConflictCost pair (teacher 0 twice on slot 0)
    // + one classConflictCost pair (class 1 twice on slot 0). Full hard = 2;
    // without either -> 1.
    {
        Timetable t = makeGrid(grid);
        addLesson(t, 0, 0, {0}, 0);
        addLesson(t, 1, 1, {0}, 0);
        addLesson(t, 1, 2, {1}, 0);
        ScoreCalculator::prepare(t);

        DecomposedCalculator full;
        assembleExcept(full, kZeroWeights, "");
        full.resetWorkingSolution(t);
        checkCount("case1 full assembly hard", -full.calculateScore().hard, 2);

        DecomposedCalculator noTeacherConflict;
        assembleExcept(noTeacherConflict, kZeroWeights, "teacherConflictCost");
        noTeacherConflict.resetWorkingSolution(t);
        checkCount("case1 without teacherConflictCost: the class pair",
                   -noTeacherConflict.calculateScore().hard,
                   1);

        DecomposedCalculator noClassConflict;
        assembleExcept(noClassConflict, kZeroWeights, "classConflictCost");
        noClassConflict.resetWorkingSolution(t);
        checkCount("case1 without classConflictCost: the teacher pair",
                   -noClassConflict.calculateScore().hard,
                   1);
    }

    // Case 2 (soft): the forced-gap shape — one class, periods 1 and 5 on
    // one day (courseDailyLimit 2). Default weights: teacherGapCost 20*3 +
    // classGapCost 10*3 + teacherWorkdayCost 3*1 = 93. Without
    // teacherGapCost -> 33.
    {
        Timetable t = makeGrid(grid);
        addLesson(t, 0, 0, {0}, slotIdx(grid, 0, 1));
        addLesson(t, 0, 0, {0}, slotIdx(grid, 0, 5));
        t.courseDailyLimit = {2};
        ScoreCalculator::prepare(t);

        DecomposedCalculator full;
        assembleExcept(full, ObjectiveWeights{}, "");
        full.resetWorkingSolution(t);
        const Score fullScore = full.calculateScore();
        checkCount("case2 full assembly soft penalty",
                   static_cast<std::int64_t>(-fullScore.soft),
                   93);
        checkCount("case2 full assembly hard", -fullScore.hard, 0);

        DecomposedCalculator noTeacherGap;
        assembleExcept(noTeacherGap, ObjectiveWeights{}, "teacherGapCost");
        noTeacherGap.resetWorkingSolution(t);
        checkCount(
            "case2 without teacherGapCost: 93 - 20*3",
            static_cast<std::int64_t>(-noTeacherGap.calculateScore().soft),
            33);
    }

    // Case 3 (global hard): two unassigned lessons cost unassignedLessonsCost
    // = 2; dropping it leaves nothing else violated.
    {
        Timetable t = makeGrid(grid);
        addLesson(t, 0, 0, {0});
        addLesson(t, 0, 1, {1});
        addLesson(t, 0, 2, {2}, 0);
        ScoreCalculator::prepare(t);

        DecomposedCalculator full;
        assembleExcept(full, kZeroWeights, "");
        full.resetWorkingSolution(t);
        checkCount("case3 full assembly hard (unassignedLessonsCost x2)",
                   -full.calculateScore().hard,
                   2);

        DecomposedCalculator noUnassigned;
        assembleExcept(noUnassigned, kZeroWeights, "unassignedLessonsCost");
        noUnassigned.resetWorkingSolution(t);
        checkCount("case3 without unassignedLessonsCost: hard clean",
                   -noUnassigned.calculateScore().hard,
                   0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides: the level and weight are assembly-time knobs.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) weight / level overrides ==\n");
    const std::vector<int> grid{5, 5};

    // teacherMaxPeriodsCost demoted to soft at weight 7: one daily-excess
    // point (3 periods over maxPerDay 2) moves from hard to soft * 7.
    {
        Timetable t = makeGrid(grid);
        t.teacherMaxPerDay = {2};
        addLesson(t, 0, 0, {0}, slotIdx(grid, 0, 1));
        addLesson(t, 1, 1, {0}, slotIdx(grid, 0, 3));
        addLesson(t, 2, 2, {0}, slotIdx(grid, 0, 5));
        ScoreCalculator::prepare(t);

        DecomposedCalculator hardCalc;
        assembleExcept(hardCalc, kZeroWeights, "");
        hardCalc.resetWorkingSolution(t);
        const Score hardScore = hardCalc.calculateScore();
        checkCount(
            "teacherMaxPeriodsCost default: 1 hard point", -hardScore.hard, 1);
        checkCount("teacherMaxPeriodsCost default: soft clean",
                   static_cast<std::int64_t>(-hardScore.soft),
                   0);

        DecomposedCalculator softCalc;
        assembleExcept(softCalc, kZeroWeights, "teacherMaxPeriodsCost");
        auto maxPeriods = std::make_unique<TeacherMaxPeriodsCost>(7.0);
        maxPeriods->level = optimaru::Level::Soft;  // same cost, soft level
        softCalc.add(std::move(maxPeriods));
        softCalc.resetWorkingSolution(t);
        const Score softScore = softCalc.calculateScore();
        checkCount(
            "teacherMaxPeriodsCost demoted: hard clean", -softScore.hard, 0);
        checkCount("teacherMaxPeriodsCost demoted: soft = 7 * 1",
                   static_cast<std::int64_t>(-softScore.soft),
                   7);
    }

    // teacherGapCost at double weight on the forced-gap shape: 93 + 20*3 =
    // 153.
    {
        Timetable t = makeGrid(grid);
        addLesson(t, 0, 0, {0}, slotIdx(grid, 0, 1));
        addLesson(t, 0, 0, {0}, slotIdx(grid, 0, 5));
        t.courseDailyLimit = {2};
        ScoreCalculator::prepare(t);

        DecomposedCalculator calc;
        assembleExcept(calc, ObjectiveWeights{}, "teacherGapCost");
        calc.emplace<TeacherGapCost>(2.0 * TeacherGapCost::kDefaultWeight);
        calc.resetWorkingSolution(t);
        checkCount("teacherGapCost doubled: 93 + 20*3",
                   static_cast<std::int64_t>(-calc.calculateScore().soft),
                   153);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) FULL_ASSERT solve on the plugin path: the engine cross-checks the
// decomposed incremental score against a full reset + recompute after every
// move (custom move operators included) and throws ScoreCorruptionError on
// any divergence.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (e) FULL_ASSERT plugin solve ==\n");
    Timetable t = buildRandomInstance(/*courseCount*/ 8, /*seed*/ 11);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    // Kick early so the LNS ruin/recreate path also runs under FULL_ASSERT.
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    DecomposedCalculator calc;
    addCanonicalConstraints(calc);

    bool corrupted = false;
    Score final{};
    try
    {
        Solver solver(config);
        solver.solve(t, calc);
        final = t.score;
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        // Both sides at the default weights, so the plugin score must equal
        // the monolithic full recompute bit-for-bit.
        const Score recomputed
            = ScoreCalculator::computeBreakdown(t).toScore(ObjectiveWeights{});
        check(final.hard == recomputed.hard && final.soft == recomputed.soft,
              "plugin-reported score == monolithic full recompute");
        check(calc.calculateScore().hard == final.hard
                  && calc.calculateScore().soft == final.soft,
              "calculator left primed on the final solution");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) CP-SAT-sample-scale demo school (the canonical_test (e) instance):
// 5 days (7/7/7/7/6 periods = 34 slots), 6 classes x 34 weekly periods, 15
// teachers, special rooms, art double periods, the merged tech group, team
// teaching and pinned homeroom hours — solved on the plugin path.
// -------------------------------------------------------------------------
struct DemoSchool
{
    Timetable t;
    std::vector<int> hrLessons;               // pinned, slot 0
    std::vector<std::vector<int>> artBlocks;  // per class: 2 lesson indices
    std::vector<int> mergedTech;              // 2 lesson indices (class C11)
    int teamLesson = -1;
    int nextCourse = 0;
    int nextBlock = 0;
};

DemoSchool buildDemoSchool()
{
    // Teacher ids: T01..T15 -> 0..14. Class ids: C11,C12,C21,C22,C31,C32 ->
    // 0..5. Room types: 0 SCI(2), 1 MUS(1), 2 ART(1), 3 GYM(1).
    const std::vector<int> ppd{7, 7, 7, 7, 6};
    DemoSchool d;
    d.t = makeGrid(ppd);
    const int slotCount = d.t.slotCount();
    d.t.teacherCount = 15;
    d.t.classCount = 6;
    d.t.roomCapacityByType = {2, 1, 1, 1};

    d.t.teacherMaxPerWeek.assign(15, -1);
    d.t.teacherMaxPerWeek[0] = 18;   // T01
    d.t.teacherMaxPerWeek[10] = 20;  // T11
    d.t.teacherPreferredFreeDays.assign(15, 0);
    d.t.teacherPreferredFreeDays[3] = 1 << 4;   // T04: Friday
    d.t.teacherPreferredFreeDays[12] = 1 << 4;  // T13: Friday

    // Teacher unavailability, baked into the lessons' allowedSlot tables.
    std::vector<std::vector<char>> teacherAllowed(
        15, std::vector<char>(slotCount, 1));
    teacherAllowed[1][slotIdx(ppd, 0, 7)] = 0;  // T02: Mon 7
    teacherAllowed[1][slotIdx(ppd, 1, 7)] = 0;  // T02: Tue 7
    teacherAllowed[3][slotIdx(ppd, 2, 6)] = 0;  // T04: Wed 6
    teacherAllowed[3][slotIdx(ppd, 2, 7)] = 0;  // T04: Wed 7
    for (int p = 1; p <= 6; ++p)
        teacherAllowed[11][slotIdx(ppd, 4, p)] = 0;  // T12: all of Friday

    auto addCourse = [&](int classId,
                         std::vector<int> teachers,
                         int periods,
                         int dailyLimit,
                         int roomType,
                         std::uint16_t avoided,
                         std::uint16_t preferred,
                         bool block)
    {
        const int key = d.nextCourse++;
        d.t.courseDailyLimit.push_back(dailyLimit);
        d.t.courseAvoidedPeriodMask.push_back(avoided);
        d.t.coursePreferredPeriodMask.push_back(preferred);
        const int group = block ? d.nextBlock++ : -1;
        std::vector<char> allowed(slotCount, 1);
        for (int teacher : teachers)
        {
            for (int s = 0; s < slotCount; ++s)
            {
                if (teacherAllowed[teacher][s] == 0)
                    allowed[s] = 0;
            }
        }
        std::vector<int> made;
        for (int i = 0; i < periods; ++i)
        {
            const int lesson = addLesson(d.t, classId, key, teachers);
            d.t.lessons[lesson].allowedSlot = allowed;
            d.t.lessons[lesson].blockGroup = group;
            if (roomType >= 0)
            {
                d.t.lessons[lesson].roomTypeDemand.assign(4, 0);
                d.t.lessons[lesson].roomTypeDemand[roomType] = 1;
            }
            made.push_back(lesson);
        }
        return made;
    };

    // Subject -> per-class teacher (class order C11..C32), from sample.py.
    const int KOR[6] = {0, 0, 0, 1, 1, 1};
    const int MATH[6] = {2, 2, 2, 3, 3, 3};
    const int ENG[6] = {4, 5, 4, 5, 4, 5};
    const int SCI[6] = {7, 7, 6, 6, 6, 7};
    const int SOC[6] = {8, 8, 9, 9, 8, 9};
    const int TECH[6] = {-1, 13, 13, 14, 13, 14};  // C11 is the merged group
    const int HOMEROOM[6] = {0, 2, 4, 6, 8, 10};

    for (int c = 0; c < 6; ++c)
    {
        addCourse(c, {KOR[c]}, 5, 1, -1, 0, 0, false);
        addCourse(
            c, {MATH[c]}, 5, 1, -1, 0, 0x000F, false);  // prefers mornings
        addCourse(c, {ENG[c]}, c == 2 ? 4 : 5, 1, -1, 0, 0, false);
        addCourse(c, {SCI[c]}, 4, 1, /*SCI*/ 0, 0, 0, false);
        addCourse(c, {SOC[c]}, 4, 1, -1, 0, 0, false);
        addCourse(c, {SOC[c]}, 1, 1, -1, 0, 0, false);  // ETH, same teacher
        addCourse(
            c, {10}, 3, 1, /*GYM*/ 3, 0x0001, 0, false);  // PE, avoids period 1
        addCourse(c, {11}, 2, 1, /*MUS*/ 1, 0, 0, false);
        d.artBlocks.push_back(
            addCourse(c, {12}, 2, 2, /*ART*/ 2, 0, 0, true));  // 2-period block
        if (c == 0)
            d.mergedTech = addCourse(0, {13, 14}, 2, 1, -1, 0, 0, false);
        else
            addCourse(c, {TECH[c]}, 2, 1, -1, 0, 0, false);
        // Homeroom hour (creative activities): pinned to Monday period 1.
        const std::vector<int> hr
            = addCourse(c, {HOMEROOM[c]}, 1, 1, -1, 0, 0, false);
        d.t.lessons[hr[0]].pinned = true;
        d.t.lessons[hr[0]].slot = slotIdx(ppd, 0, 1);
        d.hrLessons.push_back(hr[0]);
    }
    // Team teaching: one C21 English conversation hour with both teachers.
    d.teamLesson = addCourse(2, {4, 5}, 1, 1, -1, 0, 0, false)[0];
    return d;
}

void verifySampleScaleSolve()
{
    std::printf("== (g) Plugin solve: sample-scale hard 0 in 20s ==\n");
    DemoSchool d = buildDemoSchool();
    std::printf("  instance: %d lessons, %d slots, %d teachers, %d classes\n",
                static_cast<int>(d.t.lessons.size()),
                d.t.slotCount(),
                15,
                6);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(20);
    config.bestScoreLimit = Score{0, -std::numeric_limits<double>::infinity()};
    config.randomSeed = 1;
    config.weights = ObjectiveWeights{};  // ignored on the plugin path

    DecomposedCalculator calc;
    addCanonicalConstraints(calc);

    const auto start = std::chrono::steady_clock::now();
    Solver solver(config);
    solver.solve(d.t, calc);
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - start)
                               .count();

    const Breakdown b = ScoreCalculator::computeBreakdown(d.t);
    std::printf(
        "  solved in %.1fs, score %s\n", seconds, d.t.score.toString().c_str());
    checkCount("sample-scale hard total", b.hardTotal(), 0);
    check(seconds <= 20.5, "finished within the 20s budget");
    check(d.t.score.hard == -b.hardTotal()
              && d.t.score.soft == -b.softPenalty(ObjectiveWeights{}),
          "plugin score matches the monolithic recompute");

    bool hrPinned = true;
    for (int i : d.hrLessons)
        hrPinned = hrPinned && d.t.lessons[i].slot == 0;
    check(hrPinned, "homeroom hours stayed pinned to Monday period 1");
    std::printf("\n");
}

// An over-constrained instance: one class, 30 lessons of 6 courses, but only
// 20 slots. Nothing can place all of them — the question is what the search
// does about it.
Timetable buildOverConstrained()
{
    const std::vector<int> ppd{4, 4, 4, 4, 4};  // 20 slots
    Timetable t = makeGrid(ppd);
    t.classCount = 1;
    t.teacherCount = 1;
    for (int course = 0; course < 6; ++course)
    {
        t.courseDailyLimit.push_back(5);  // not the binding limit
        for (int i = 0; i < 5; ++i)
            addLesson(t, 0, course, {0});
    }
    t.teacherMaxPerDay = {20};
    t.teacherMaxPerWeek = {-1};
    t.teacherMaxConsecutive = {20};
    t.teacherPreferredFreeDays = {0};
    return t;
}

void verifyRelaxedUnassigned()
{
    std::printf("== (f) unassignedLessonsCost soft: unassigning becomes a "
                "choice ==\n");

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(3);
    config.randomSeed = 1;
    config.weights = kZeroWeights;

    // unassignedLessonsCost hard (the default): every lesson takes a slot and
    // the leftovers show up as class conflicts — the search has no way to
    // leave one out.
    Timetable strict = buildOverConstrained();
    DecomposedCalculator hard;
    weightedDefaults(kZeroWeights).into(hard);
    Solver(config).solve(strict, hard);
    const Breakdown strictBreakdown = ScoreCalculator::computeBreakdown(strict);
    checkCount("unassignedLessonsCost hard: nothing left unassigned",
               strictBreakdown.unassignedLessonsCost,
               0);
    check(strictBreakdown.classConflictCost > 0,
          "unassignedLessonsCost hard: the overflow lands on classConflictCost "
          "instead");

    // unassignedLessonsCost soft at its documented soft weight: the 10
    // lessons that do not fit come out, and what stays is conflict-free — the
    // CP-SAT UnassignedLessonsCost(hard=False) contract.
    Timetable partial = buildOverConstrained();
    DecomposedCalculator soft;
    weightedDefaults(kZeroWeights)
        .soft("unassignedLessonsCost")
        .weight("unassignedLessonsCost", UnassignedLessonsCost::kSoftWeight)
        .into(soft);
    Solver(config).solve(partial, soft);
    const Breakdown breakdown = ScoreCalculator::computeBreakdown(partial);
    checkCount(
        "unassignedLessonsCost soft: 30 lessons - 20 slots left unassigned",
        breakdown.unassignedLessonsCost,
        10);
    checkCount("unassignedLessonsCost soft: what stayed is conflict-free",
               breakdown.hardTotal() - breakdown.unassignedLessonsCost,
               0);
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) Raw costs by hand, through constraintCosts(): one positive and one
// zero case per constraint (before weight, exponent and level).
// -------------------------------------------------------------------------
using Build = std::function<void(Timetable &)>;
const std::vector<int> kGridRaw{5, 5};

// The raw total (sum of the unit costs) the default assembly reports for
// `name` on the instance `build` makes from a 2-day x 5-period grid.
double rawCost(const Build &build, const std::string &name)
{
    Timetable t = makeGrid(kGridRaw);
    build(t);
    ScoreCalculator::prepare(t);
    DecomposedCalculator calc;
    addDefaultConstraints(calc);
    calc.resetWorkingSolution(t);
    calc.calculateScore();  // primes the per-unit cache
    for (const auto &entry : calc.constraintCosts())
    {
        if (entry.name != name)
            continue;
        double total = 0.0;
        for (double cost : entry.unitCosts)
            total += cost;
        return total;
    }
    return -1.0;  // not in the default assembly
}

void checkRaw(const std::string &name,
              const Build &positive,
              double expected,
              const Build &zero)
{
    const double hit = rawCost(positive, name);
    const double clean = rawCost(zero, name);
    std::printf("  [%s] %-52s positive=%g (expected %g) zero=%g\n",
                hit == expected && clean == 0.0 ? "PASS" : "FAIL",
                name.c_str(),
                hit,
                expected,
                clean);
    if (hit != expected || clean != 0.0)
        ++failures;
}

int slotOf(int day, int period) { return slotIdx(kGridRaw, day, period); }

void verifyRawCostsByHand()
{
    std::printf("== (h) Raw costs by hand, per constraint ==\n");
    // Two lessons, one of them without a slot -> 1; both placed -> 0.
    checkRaw(
        "unassignedLessonsCost",
        [](Timetable &t)
        {
            addLesson(t, 0, 0, {0}, slotOf(0, 1));
            addLesson(t, 0, 1, {1});
        },
        1.0,
        [](Timetable &t)
        {
            addLesson(t, 0, 0, {0}, slotOf(0, 1));
            addLesson(t, 0, 1, {1}, slotOf(0, 2));
        });
    // Class 0 twice on Mon 1 -> one pair; on Mon 1 and Mon 2 -> none.
    checkRaw(
        "classConflictCost",
        [](Timetable &t)
        {
            addLesson(t, 0, 0, {0}, slotOf(0, 1));
            addLesson(t, 0, 1, {1}, slotOf(0, 1));
        },
        1.0,
        [](Timetable &t)
        {
            addLesson(t, 0, 0, {0}, slotOf(0, 1));
            addLesson(t, 0, 1, {1}, slotOf(0, 2));
        });
    // Two team-taught lessons sharing both teachers on one slot -> one pair
    // per shared teacher = 2; one teacher each -> 0.
    checkRaw(
        "teacherConflictCost",
        [](Timetable &t)
        {
            addLesson(t, 0, 0, {0, 1}, slotOf(0, 1));
            addLesson(t, 1, 1, {0, 1}, slotOf(0, 1));
        },
        2.0,
        [](Timetable &t)
        {
            addLesson(t, 0, 0, {0}, slotOf(0, 1));
            addLesson(t, 1, 1, {1}, slotOf(0, 1));
        });
    // A lesson on a slot its allowedSlot table forbids -> 1; elsewhere -> 0.
    checkRaw(
        "disallowedSlotCost",
        [](Timetable &t)
        {
            const int i = addLesson(t, 0, 0, {0}, slotOf(0, 1));
            t.lessons[i].allowedSlot.assign(10, 1);
            t.lessons[i].allowedSlot[slotOf(0, 1)] = 0;
        },
        1.0,
        [](Timetable &t)
        {
            const int i = addLesson(t, 0, 0, {0}, slotOf(0, 2));
            t.lessons[i].allowedSlot.assign(10, 1);
            t.lessons[i].allowedSlot[slotOf(0, 1)] = 0;
        });
    // One lab, three lab lessons on Mon 1 -> 3 - 1 = 2; spread -> 0.
    checkRaw(
        "roomCapacityCost",
        [](Timetable &t)
        {
            t.roomCapacityByType = {1};
            for (int c = 0; c < 3; ++c)
            {
                const int i = addLesson(t, c, c, {c}, slotOf(0, 1));
                t.lessons[i].roomTypeDemand = {1};
            }
        },
        2.0,
        [](Timetable &t)
        {
            t.roomCapacityByType = {1};
            for (int c = 0; c < 3; ++c)
            {
                const int i = addLesson(t, c, c, {c}, slotOf(0, c + 1));
                t.lessons[i].roomTypeDemand = {1};
            }
        });
    // Daily cap 1, three lessons of the course on Monday -> 2; one per day
    // (two days) -> 0.
    checkRaw(
        "courseMaxPeriodsPerDayCost",
        [](Timetable &t)
        {
            t.courseDailyLimit = {1};
            for (int p = 1; p <= 3; ++p)
                addLesson(t, 0, 0, {0}, slotOf(0, p));
        },
        2.0,
        [](Timetable &t)
        {
            t.courseDailyLimit = {1};
            addLesson(t, 0, 0, {0}, slotOf(0, 1));
            addLesson(t, 0, 0, {0}, slotOf(1, 1));
        });
    // A 2-period block on Mon 1 and Mon 3 -> 1; on Mon 1-2 -> 0.
    checkRaw(
        "blockAdjacencyCost",
        [](Timetable &t)
        {
            t.courseDailyLimit = {2};
            const int a = addLesson(t, 0, 0, {0}, slotOf(0, 1));
            const int b = addLesson(t, 0, 0, {0}, slotOf(0, 3));
            t.lessons[a].blockGroup = 0;
            t.lessons[b].blockGroup = 0;
        },
        1.0,
        [](Timetable &t)
        {
            t.courseDailyLimit = {2};
            const int a = addLesson(t, 0, 0, {0}, slotOf(0, 1));
            const int b = addLesson(t, 0, 0, {0}, slotOf(0, 2));
            t.lessons[a].blockGroup = 0;
            t.lessons[b].blockGroup = 0;
        });
    // maxPerDay 2 and maxPerWeek 3, four lessons: three on Monday (daily
    // excess 1) plus one on Tuesday (weekly 4 - 3 = 1) -> 2; caps 3 / 4 -> 0.
    checkRaw(
        "teacherMaxPeriodsCost",
        [](Timetable &t)
        {
            t.teacherMaxPerDay = {2};
            t.teacherMaxPerWeek = {3};
            for (int p = 1; p <= 3; ++p)
                addLesson(t, p - 1, p - 1, {0}, slotOf(0, 2 * p - 1));
            addLesson(t, 3, 3, {0}, slotOf(1, 1));
        },
        2.0,
        [](Timetable &t)
        {
            t.teacherMaxPerDay = {3};
            t.teacherMaxPerWeek = {4};
            for (int p = 1; p <= 3; ++p)
                addLesson(t, p - 1, p - 1, {0}, slotOf(0, 2 * p - 1));
            addLesson(t, 3, 3, {0}, slotOf(1, 1));
        });
    // Limit 2: a run of Mon 1-4 -> 4 - 2 = 2; runs of 2 and 2 (Mon 1-2,
    // 4-5) -> 0.
    checkRaw(
        "teacherMaxConsecutivePeriodsCost",
        [](Timetable &t)
        {
            t.teacherMaxConsecutive = {2};
            for (int p = 1; p <= 4; ++p)
                addLesson(t, p - 1, p - 1, {0}, slotOf(0, p));
        },
        2.0,
        [](Timetable &t)
        {
            t.teacherMaxConsecutive = {2};
            int c = 0;
            for (int p : {1, 2, 4, 5})
            {
                addLesson(t, c, c, {0}, slotOf(0, p));
                ++c;
            }
        });
    // Teacher on Mon 1 and Mon 4 -> 2 idle periods; Mon 1 and 2 -> 0.
    checkRaw(
        "teacherGapCost",
        [](Timetable &t)
        {
            addLesson(t, 0, 0, {0}, slotOf(0, 1));
            addLesson(t, 1, 1, {0}, slotOf(0, 4));
        },
        2.0,
        [](Timetable &t)
        {
            addLesson(t, 0, 0, {0}, slotOf(0, 1));
            addLesson(t, 1, 1, {0}, slotOf(0, 2));
        });
    // One teacher on both days -> 2 workdays; nothing placed -> 0.
    checkRaw(
        "teacherWorkdayCost",
        [](Timetable &t)
        {
            addLesson(t, 0, 0, {0}, slotOf(0, 1));
            addLesson(t, 0, 1, {0}, slotOf(1, 1));
        },
        2.0,
        [](Timetable &t)
        {
            addLesson(t, 0, 0, {0});
            addLesson(t, 0, 1, {0});
        });
    // Tuesday requested free, a lesson on Tuesday -> 1 worked requested free
    // day; on Monday -> 0.
    checkRaw(
        "teacherFreeDayCost",
        [](Timetable &t)
        {
            t.teacherPreferredFreeDays = {1 << 1};
            addLesson(t, 0, 0, {0}, slotOf(1, 2));
        },
        1.0,
        [](Timetable &t)
        {
            t.teacherPreferredFreeDays = {1 << 1};
            addLesson(t, 0, 0, {0}, slotOf(0, 2));
        });
    // Class on Mon 2 and Mon 5 (two teachers) -> 2 idle periods; Mon 2-3 ->
    // 0.
    checkRaw(
        "classGapCost",
        [](Timetable &t)
        {
            addLesson(t, 0, 0, {0}, slotOf(0, 2));
            addLesson(t, 0, 1, {1}, slotOf(0, 5));
        },
        2.0,
        [](Timetable &t)
        {
            addLesson(t, 0, 0, {0}, slotOf(0, 2));
            addLesson(t, 0, 1, {1}, slotOf(0, 3));
        });
    // A merged lesson with two units, both avoiding period 1, on Mon 1 -> 2;
    // on Mon 2 -> 0.
    checkRaw(
        "avoidedPeriodCost",
        [](Timetable &t)
        {
            const int i = addLesson(t, 0, 0, {0, 1}, slotOf(0, 1));
            t.lessons[i].preferenceUnits = {{0x0001, 0}, {0x0001, 0}};
        },
        2.0,
        [](Timetable &t)
        {
            const int i = addLesson(t, 0, 0, {0, 1}, slotOf(0, 2));
            t.lessons[i].preferenceUnits = {{0x0001, 0}, {0x0001, 0}};
        });
    // Subject preferring period 2, lesson on Mon 1 -> 1; on Mon 2 -> 0.
    checkRaw(
        "nonPreferredPeriodCost",
        [](Timetable &t)
        {
            t.coursePreferredPeriodMask = {0x0002};
            addLesson(t, 0, 0, {0}, slotOf(0, 1));
        },
        1.0,
        [](Timetable &t)
        {
            t.coursePreferredPeriodMask = {0x0002};
            addLesson(t, 0, 0, {0}, slotOf(0, 2));
        });
    // The two structural constraints are 0 by construction, so their
    // "positive" case is the worst the model can express: a pinned lesson
    // sitting on its fixed slot next to a clash, and a merged parallel group
    // (one lesson for two sections) on a disallowed slot. Both stay 0 — the
    // clash and the disallowed slot are other constraints' costs.
    checkRaw(
        "fixedSlotsCost",
        [](Timetable &t)
        {
            const int i = addLesson(t, 0, 0, {0}, slotOf(0, 1));
            t.lessons[i].pinned = true;
            addLesson(t, 0, 1, {0}, slotOf(0, 1));
        },
        0.0,
        [](Timetable &t)
        {
            const int i = addLesson(t, 0, 0, {0}, slotOf(0, 1));
            t.lessons[i].pinned = true;
        });
    checkRaw(
        "parallelGroupsCost",
        [](Timetable &t)
        {
            const int i = addLesson(t, 0, 0, {0, 1}, slotOf(0, 1));
            t.lessons[i].preferenceUnits = {{0, 0}, {0, 0}};
            t.lessons[i].allowedSlot.assign(10, 1);
            t.lessons[i].allowedSlot[slotOf(0, 1)] = 0;
        },
        0.0,
        [](Timetable &t)
        {
            const int i = addLesson(t, 0, 0, {0, 1}, slotOf(0, 2));
            t.lessons[i].preferenceUnits = {{0, 0}, {0, 0}};
        });
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The catalog: info() rows equal every component's constants, in the
// recorded catalog order; the structural constraints are not flippable.
// -------------------------------------------------------------------------
template <typename Component>
bool rowIs(const optimaru::ConstraintInfo &row,
           const char *name,
           optimaru::Level level,
           double weight,
           bool flippable = true)
{
    return row.name == name && row.defaultLevel == Component::kDefaultLevel
           && row.defaultWeight == Component::kDefaultWeight
           && row.defaultLevel == level && row.defaultWeight == weight
           && row.levelFlippable == flippable && !row.reward
           && row.defaultEnabled;
}

template <typename F> bool throwsInvalidArgument(F &&f)
{
    try
    {
        f();
    }
    catch (const std::invalid_argument &)
    {
        return true;
    }
    return false;
}

void verifyCatalog()
{
    std::printf("== (i) Catalog: info() == the components' constants ==\n");
    using optimaru::Level;
    const std::vector<std::string> order = constraintOrder();
    const std::vector<std::string> expected{"unassignedLessonsCost",
                                            "classConflictCost",
                                            "teacherConflictCost",
                                            "disallowedSlotCost",
                                            "roomCapacityCost",
                                            "courseMaxPeriodsPerDayCost",
                                            "blockAdjacencyCost",
                                            "teacherMaxPeriodsCost",
                                            "teacherMaxConsecutivePeriodsCost",
                                            "teacherGapCost",
                                            "teacherWorkdayCost",
                                            "teacherFreeDayCost",
                                            "classGapCost",
                                            "avoidedPeriodCost",
                                            "nonPreferredPeriodCost",
                                            "fixedSlotsCost",
                                            "parallelGroupsCost"};
    check(order == expected, "constraintOrder() is the catalog order");

    const std::vector<optimaru::ConstraintInfo> rows
        = defaultConstraints().info();
    std::map<std::string, optimaru::ConstraintInfo> byName;
    for (const optimaru::ConstraintInfo &row : rows)
        byName.emplace(row.name, row);
    bool sameSet = rows.size() == order.size();
    for (const std::string &name : order)
        sameSet = sameSet && byName.count(name) == 1;
    check(sameSet, "constraintOrder() names exactly the default assembly");
    if (!sameSet)
        return;

    auto row = [&byName](const char *name) { return byName.at(name); };
    check(
        rowIs<UnassignedLessonsCost>(row("unassignedLessonsCost"),
                                     "unassignedLessonsCost",
                                     Level::Hard,
                                     1.0)
            && rowIs<ClassConflictCost>(
                row("classConflictCost"), "classConflictCost", Level::Hard, 1.0)
            && rowIs<TeacherConflictCost>(row("teacherConflictCost"),
                                          "teacherConflictCost",
                                          Level::Hard,
                                          1.0)
            && rowIs<DisallowedSlotCost>(row("disallowedSlotCost"),
                                         "disallowedSlotCost",
                                         Level::Hard,
                                         1.0)
            && rowIs<RoomCapacityCost>(
                row("roomCapacityCost"), "roomCapacityCost", Level::Hard, 1.0)
            && rowIs<CourseMaxPeriodsPerDayCost>(
                row("courseMaxPeriodsPerDayCost"),
                "courseMaxPeriodsPerDayCost",
                Level::Hard,
                1.0)
            && rowIs<BlockAdjacencyCost>(row("blockAdjacencyCost"),
                                         "blockAdjacencyCost",
                                         Level::Hard,
                                         1.0)
            && rowIs<TeacherMaxPeriodsCost>(row("teacherMaxPeriodsCost"),
                                            "teacherMaxPeriodsCost",
                                            Level::Hard,
                                            1.0)
            && rowIs<TeacherMaxConsecutivePeriodsCost>(
                row("teacherMaxConsecutivePeriodsCost"),
                "teacherMaxConsecutivePeriodsCost",
                Level::Hard,
                1.0),
        "the nine hard rows: hard, weight 1, flippable");
    check(
        rowIs<TeacherGapCost>(
            row("teacherGapCost"), "teacherGapCost", Level::Soft, 20.0)
            && rowIs<TeacherWorkdayCost>(row("teacherWorkdayCost"),
                                         "teacherWorkdayCost",
                                         Level::Soft,
                                         3.0)
            && rowIs<TeacherFreeDayCost>(row("teacherFreeDayCost"),
                                         "teacherFreeDayCost",
                                         Level::Soft,
                                         30.0)
            && rowIs<ClassGapCost>(
                row("classGapCost"), "classGapCost", Level::Soft, 10.0)
            && rowIs<AvoidedPeriodCost>(
                row("avoidedPeriodCost"), "avoidedPeriodCost", Level::Soft, 5.0)
            && rowIs<NonPreferredPeriodCost>(row("nonPreferredPeriodCost"),
                                             "nonPreferredPeriodCost",
                                             Level::Soft,
                                             1.0),
        "the six soft rows: 20/3/30/10/5/1, flippable");
    check(rowIs<FixedSlotsCost>(row("fixedSlotsCost"),
                                "fixedSlotsCost",
                                Level::Hard,
                                1.0,
                                /*flippable=*/false)
              && rowIs<ParallelGroupsCost>(row("parallelGroupsCost"),
                                           "parallelGroupsCost",
                                           Level::Hard,
                                           1.0,
                                           /*flippable=*/false),
          "the two structural rows: hard, weight 1, not flippable");

    for (const char *name : {"fixedSlotsCost", "parallelGroupsCost"})
    {
        check(
            throwsInvalidArgument([name] { defaultConstraints().soft(name); }),
            std::string(name) + ": soft() is refused");
        check(
            !throwsInvalidArgument([name] { defaultConstraints().hard(name); }),
            std::string(name) + ": hard() is a no-op");
        check(throwsInvalidArgument(
                  [name] { defaultConstraints().weight(name, -1.0); }),
              std::string(name) + ": a negative weight is refused");
    }
    // Every other constraint flips both ways.
    bool flippable = true;
    for (const std::string &name : order)
    {
        if (name == "fixedSlotsCost" || name == "parallelGroupsCost")
            continue;
        flippable = flippable
                    && !throwsInvalidArgument(
                        [&name]
                        {
                            defaultConstraints().soft(name);
                            defaultConstraints().hard(name);
                        });
    }
    check(flippable, "the fifteen other constraints flip both ways");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) Repeated components and offsets. A repeat is a fresh copy of a default
// component (another defaultConstraints(hub), the one name kept, its knobs
// re-stated) appended with addInstances(), as the Python bindings build it
// from a pick list — every copy built on the SAME hub, so the teacher-mask
// instances (repeats included) subscribe to one projection.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string &name,
                          const std::shared_ptr<TeacherMaskHub> &hub)
{
    ConstraintAssembly copy = defaultConstraints(hub);
    copy.only({name});
    return copy;
}

// 1 day x 4 periods, one teacher and one class, lessons in periods 1 and 3:
// teacherGapCost 1, teacherWorkdayCost 1, classGapCost 1, everything else 0.
Timetable repeatInstance()
{
    const std::vector<int> grid{4};
    Timetable t = makeGrid(grid);
    addLesson(t, 0, 0, {0}, slotIdx(grid, 0, 1));
    addLesson(t, 0, 1, {0}, slotIdx(grid, 0, 3));
    ScoreCalculator::prepare(t);
    return t;
}

ConstraintAssembly repeatedAssembly()
{
    const auto hub = std::make_shared<TeacherMaskHub>();
    ConstraintAssembly assembly = defaultConstraints(hub);
    assembly.offset("teacherGapCost", 1.0);
    assembly.addInstances(copyOf("teacherGapCost", hub)
                              .weight("teacherGapCost", 0.5)
                              .exponent("teacherGapCost", 2.0)
                              .offset("teacherGapCost", 2.0)
                              .clip("teacherGapCost", false));
    assembly.addInstances(copyOf("teacherMaxPeriodsCost", hub)
                              .weight("teacherMaxPeriodsCost", 2.0)
                              .offset("teacherMaxPeriodsCost", 1.0)
                              .clip("teacherMaxPeriodsCost", false));
    assembly.addInstances(copyOf("teacherWorkdayCost", hub)
                              .offset("teacherWorkdayCost", -1.0));
    assembly.addInstances(copyOf("classGapCost", hub)
                              .offset("classGapCost", 1.0)
                              .clip("classGapCost", false));
    assembly.addInstances(copyOf("avoidedPeriodCost", hub)
                              .weight("avoidedPeriodCost", 2.0)
                              .offset("avoidedPeriodCost", 1.0)
                              .clip("avoidedPeriodCost", false));
    assembly.addInstances(copyOf("teacherConflictCost", hub)
                              .offset("teacherConflictCost", 0.5)
                              .clip("teacherConflictCost", false));
    assembly.addInstances(copyOf("unassignedLessonsCost", hub)
                              .offset("unassignedLessonsCost", 1.0));
    return assembly;
}

Score scoreWithAssembly(ConstraintAssembly assembly, Timetable &t)
{
    DecomposedCalculator calc;
    std::move(assembly).into(calc);
    calc.resetWorkingSolution(t);
    return calc.calculateScore();
}

void verifyRepeatsAndOffsets()
{
    std::printf("== (j) Repeated components, offset / clip ==\n");
    {
        Timetable t = repeatInstance();
        const auto hub = std::make_shared<TeacherMaskHub>();
        ConstraintAssembly assembly = copyOf("teacherGapCost", hub);
        assembly.addInstances(copyOf("teacherGapCost", hub)
                                  .weight("teacherGapCost", 0.5)
                                  .exponent("teacherGapCost", 2.0));
        DecomposedCalculator calc;
        std::move(assembly).into(calc);
        calc.resetWorkingSolution(t);
        const Score score = calc.calculateScore();
        check(score.hard == 0 && score.soft == -(20.0 * 1 + 0.5 * 1.0),
              "teacher gaps x 20 + 0.5 x gaps^2 on one hub: -(20 + 0.5)");
        const auto entries = calc.constraintCosts();
        check(entries.size() == 2 && entries[0].unitCosts == entries[1].unitCosts
                  && entries[0].unitCosts == std::vector<double>{1.0}
                  && entries[1].weight == 0.5 && entries[1].exponent == 2.0,
              "two instances, the same raw total, each with its own knobs");
    }
    {
        Timetable t = repeatInstance();
        const auto hub = std::make_shared<TeacherMaskHub>();
        auto soft = [&](ConstraintAssembly assembly)
        { return scoreWithAssembly(std::move(assembly), t).soft; };
        auto hard = [&](ConstraintAssembly assembly)
        { return scoreWithAssembly(std::move(assembly), t).hard; };
        check(soft(copyOf("teacherGapCost", hub)
                       .offset("teacherGapCost", 0.5))
                  == -20.0 * 0.5,
              "global threshold offset 0.5: 20 x (1 - 0.5)");
        check(soft(copyOf("teacherGapCost", hub)
                       .offset("teacherGapCost", 2.0)
                       .clip("teacherGapCost", false))
                  == 20.0,
              "global literal offset 2: 20 x (1 - 2), a reward");
        check(soft(copyOf("avoidedPeriodCost", hub)
                       .offset("avoidedPeriodCost", 1.0))
                  == 0.0,
              "per-slot threshold offset 1 on four empty slots: 0");
        check(soft(copyOf("avoidedPeriodCost", hub)
                       .offset("avoidedPeriodCost", 1.0)
                       .clip("avoidedPeriodCost", false))
                  == 5.0 * 4,
              "per-slot literal offset 1: every one of the 4 slots, 5 x 1 each");
        check(soft(copyOf("teacherWorkdayCost", hub)
                       .offset("teacherWorkdayCost", -1.0))
                  == -3.0 * 2,
              "global threshold offset -1: 3 x (1 + 1)");
        // A hard literal pick rounds per slot: 3 x (0 - 0.5) = -1.5 -> -2.
        checkCount("hard per-slot literal offset 0.5 at weight 3: 4 x 2",
                   hard(copyOf("teacherConflictCost", hub)
                            .weight("teacherConflictCost", 3.0)
                            .offset("teacherConflictCost", 0.5)
                            .clip("teacherConflictCost", false)),
                   8);
    }
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        Timetable t = buildRandomInstance(/*courseCount*/ 16, /*seed*/ 23);
        ScoreCalculator::prepare(t);
        DecomposedCalculator calc;
        repeatedAssembly().into(calc);
        optimaru::ScoreDirector<Timetable> director(t, calc);
        std::mt19937_64 rng(321);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const int lessonCount = static_cast<int>(t.lessons.size());
        int mismatches = 0;
        const int steps = 2000;
        for (int step = 0; step < steps; ++step)
        {
            if (unit(rng) < 0.75)
            {
                const int lesson = static_cast<int>(rng() % lessonCount);
                const int value = unit(rng) < 0.1
                                      ? -1
                                      : static_cast<int>(rng() % t.slotCount());
                director.changeVariable(lesson, value);
            }
            else
            {
                const int a = static_cast<int>(rng() % lessonCount);
                const int b = static_cast<int>(rng() % lessonCount);
                const int va = t.lessons[a].slot;
                const int vb = t.lessons[b].slot;
                director.beginVariableChanges();
                director.changeVariable(a, vb);
                director.changeVariable(b, va);
                director.endVariableChanges();
            }
            Timetable shadow = t;
            const Score full = scoreWithAssembly(repeatedAssembly(), shadow);
            const Score incremental = director.calculateScore();
            if (!optimaru::scoresAgree(incremental, full))
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
                  + " random moves with repeated, shifted components: "
                    "incremental == fresh");
    }
    {
        Timetable t = buildRandomInstance(/*courseCount*/ 8, /*seed*/ 29);
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(2);
        config.randomSeed = 5;
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        config.ruinRecreateUnimprovedStepTrigger = 200;
        config.ruinRecreatePolishSteps = 100;
        DecomposedCalculator calc;
        repeatedAssembly().into(calc);
        bool corrupted = false;
        try
        {
            Solver solver(config);
            solver.solve(t, calc);
        }
        catch (const optimaru::ScoreCorruptionError &e)
        {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted,
              "2s FULL_ASSERT solve with repeated, shifted components");
        const Score fresh = scoreWithAssembly(repeatedAssembly(), t);
        check(!corrupted && optimaru::scoresAgree(t.score, fresh),
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
    verifyFullAssertSolve();
    verifyRelaxedUnassigned();
    verifyRawCostsByHand();
    verifyCatalog();
    verifySampleScaleSolve();
    verifyRepeatsAndOffsets();

    if (failures == 0)
    {
        std::printf("All canonical-plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d canonical-plugin check(s) FAILED.\n", failures);
    return 1;
}

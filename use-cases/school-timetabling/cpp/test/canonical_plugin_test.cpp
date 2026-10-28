// Plugin-assembly (st/canonical_constraints.hpp) tests, engine test/ style:
// check() + a non-zero exit on any failure.
//   (a) the full default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (including unassigned lessons)
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: removing one component removes exactly that
//       term, on hand-computed instances
//   (d) weight / isHard overrides: H9 demoted to soft, S1 weight doubled
//   (e) a 2-second solve(table, calculator) under FULL_ASSERT (custom move
//       operators included)
//   (f) H1 demoted to soft turns "leave it unassigned" into a move the search
//       can make (partial timetabling), and leaves it alone while H1 is hard
//   (g) the plugin-path solve reaches hard 0 on the CP-SAT-sample-scale
//       school within a 20-second budget
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "st/canonical_constraints.hpp"
#include "st/canonical_domain.hpp"
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

// Base grid: slots enumerated day-major from periodsPerDay, zero weights.
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
    t.weights = kZeroWeights;
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

// The full assembly minus one component (by constraint name; empty = all
// fifteen), S weights injected assembly-time like addCanonicalConstraints.
void assembleExcept(DecomposedCalculator &calc,
                    const ObjectiveWeights &w,
                    const std::string &skip)
{
    auto keep = [&skip](const char *name) { return skip != name; };
    if (keep("H1 unassigned lesson"))
        calc.emplace<UnassignedLesson>();
    if (keep("H3 class conflict"))
        calc.emplace<ClassConflict>();
    if (keep("H4 teacher conflict"))
        calc.emplace<TeacherConflict>();
    if (keep("H5 slot not allowed"))
        calc.emplace<SlotNotAllowed>();
    if (keep("H6 room capacity"))
        calc.emplace<RoomCapacity>();
    if (keep("H7 course daily limit"))
        calc.emplace<CourseDailyLimit>();
    if (keep("H8 block adjacency"))
        calc.emplace<BlockAdjacency>();
    if (keep("H9 teacher load limit"))
        calc.emplace<TeacherLoadLimit>();
    if (keep("H10 teacher consecutive limit"))
        calc.emplace<TeacherConsecutiveLimit>();
    if (keep("S1 teacher gap"))
        calc.emplace<TeacherGap>(static_cast<double>(w.teacherGap));
    if (keep("S2 teacher workday"))
        calc.emplace<TeacherWorkday>(static_cast<double>(w.teacherWorkday));
    if (keep("S3 teacher free day"))
        calc.emplace<TeacherFreeDay>(static_cast<double>(w.teacherFreeDay));
    if (keep("S4 class gap"))
        calc.emplace<ClassGap>(static_cast<double>(w.classGap));
    if (keep("S5 avoided period"))
        calc.emplace<AvoidedPeriod>(static_cast<double>(w.avoidedPeriod));
    if (keep("S6 non-preferred period"))
        calc.emplace<NonPreferredPeriod>(
            static_cast<double>(w.nonPreferredPeriod));
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
    t.weights = ObjectiveWeights{};  // defaults: every soft term active
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
            if (rng() % 3 == 0)  // merged-lesson style multi-unit S5/S6
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
    addCanonicalConstraints(calc, t.weights);

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
            = ScoreCalculator::computeBreakdown(t).toScore(t.weights);
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

    // The per-constraint breakdown must add up to the score it explains.
    double hardTotal = 0.0;
    double softTotal = 0.0;
    for (const optimaru::ConstraintTotal &total : calc.constraintTotals())
        (total.isHard ? hardTotal : softTotal) += total.cost;
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
    addCanonicalConstraints(calc, t.weights);
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
        const Score full
            = ScoreCalculator::computeBreakdown(shadow).toScore(t.weights);
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

    // Case 1 (hard): one H4 pair (teacher 0 twice on slot 0) + one H3 pair
    // (class 1 twice on slot 0). Full hard = 2; minus H4 -> 1; minus H3 -> 1.
    {
        Timetable t = makeGrid(grid);
        addLesson(t, 0, 0, {0}, 0);
        addLesson(t, 1, 1, {0}, 0);
        addLesson(t, 1, 2, {1}, 0);
        ScoreCalculator::prepare(t);

        DecomposedCalculator full;
        assembleExcept(full, t.weights, "");
        full.resetWorkingSolution(t);
        checkCount("case1 full assembly hard", -full.calculateScore().hard, 2);

        DecomposedCalculator noH4;
        assembleExcept(noH4, t.weights, "H4 teacher conflict");
        noH4.resetWorkingSolution(t);
        checkCount("case1 without H4: only the H3 pair",
                   -noH4.calculateScore().hard,
                   1);

        DecomposedCalculator noH3;
        assembleExcept(noH3, t.weights, "H3 class conflict");
        noH3.resetWorkingSolution(t);
        checkCount("case1 without H3: only the H4 pair",
                   -noH3.calculateScore().hard,
                   1);
    }

    // Case 2 (soft): the forced-gap shape — one class, periods 1 and 5 on
    // one day (courseDailyLimit 2). Default weights: S1 20*3 + S4 10*3 +
    // S2 3*1 = 93. Without S1 -> 33.
    {
        Timetable t = makeGrid(grid);
        t.weights = ObjectiveWeights{};
        addLesson(t, 0, 0, {0}, slotIdx(grid, 0, 1));
        addLesson(t, 0, 0, {0}, slotIdx(grid, 0, 5));
        t.courseDailyLimit = {2};
        ScoreCalculator::prepare(t);

        DecomposedCalculator full;
        assembleExcept(full, t.weights, "");
        full.resetWorkingSolution(t);
        const Score fullScore = full.calculateScore();
        checkCount("case2 full assembly soft penalty",
                   static_cast<std::int64_t>(-fullScore.soft),
                   93);
        checkCount("case2 full assembly hard", -fullScore.hard, 0);

        DecomposedCalculator noS1;
        assembleExcept(noS1, t.weights, "S1 teacher gap");
        noS1.resetWorkingSolution(t);
        checkCount("case2 without S1: 93 - 20*3",
                   static_cast<std::int64_t>(-noS1.calculateScore().soft),
                   33);
    }

    // Case 3 (global hard): two unassigned lessons cost H1 = 2; dropping H1
    // leaves nothing else violated.
    {
        Timetable t = makeGrid(grid);
        addLesson(t, 0, 0, {0});
        addLesson(t, 0, 1, {1});
        addLesson(t, 0, 2, {2}, 0);
        ScoreCalculator::prepare(t);

        DecomposedCalculator full;
        assembleExcept(full, t.weights, "");
        full.resetWorkingSolution(t);
        checkCount(
            "case3 full assembly hard (H1 x2)", -full.calculateScore().hard, 2);

        DecomposedCalculator noH1;
        assembleExcept(noH1, t.weights, "H1 unassigned lesson");
        noH1.resetWorkingSolution(t);
        checkCount(
            "case3 without H1: hard clean", -noH1.calculateScore().hard, 0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides: the level and weight are assembly-time knobs.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) weight / isHard overrides ==\n");
    const std::vector<int> grid{5, 5};

    // H9 demoted to soft at weight 7: one daily-excess point (3 periods over
    // maxPerDay 2) moves from hard to soft * 7.
    {
        Timetable t = makeGrid(grid);
        t.teacherMaxPerDay = {2};
        addLesson(t, 0, 0, {0}, slotIdx(grid, 0, 1));
        addLesson(t, 1, 1, {0}, slotIdx(grid, 0, 3));
        addLesson(t, 2, 2, {0}, slotIdx(grid, 0, 5));
        ScoreCalculator::prepare(t);

        DecomposedCalculator hardCalc;
        assembleExcept(hardCalc, t.weights, "");
        hardCalc.resetWorkingSolution(t);
        const Score hardScore = hardCalc.calculateScore();
        checkCount("H9 default: 1 hard point", -hardScore.hard, 1);
        checkCount("H9 default: soft clean",
                   static_cast<std::int64_t>(-hardScore.soft),
                   0);

        DecomposedCalculator softCalc;
        assembleExcept(softCalc, t.weights, "H9 teacher load limit");
        auto h9 = std::make_unique<TeacherLoadLimit>(/*weight=*/7.0);
        h9->isHard = false;  // demote: same cost, soft level
        softCalc.add(std::move(h9));
        softCalc.resetWorkingSolution(t);
        const Score softScore = softCalc.calculateScore();
        checkCount("H9 demoted: hard clean", -softScore.hard, 0);
        checkCount("H9 demoted: soft = 7 * 1",
                   static_cast<std::int64_t>(-softScore.soft),
                   7);
    }

    // S1 at double weight on the forced-gap shape: 93 + 20*3 = 153.
    {
        Timetable t = makeGrid(grid);
        t.weights = ObjectiveWeights{};
        addLesson(t, 0, 0, {0}, slotIdx(grid, 0, 1));
        addLesson(t, 0, 0, {0}, slotIdx(grid, 0, 5));
        t.courseDailyLimit = {2};
        ScoreCalculator::prepare(t);

        DecomposedCalculator calc;
        assembleExcept(calc, t.weights, "S1 teacher gap");
        calc.emplace<TeacherGap>(2.0 * t.weights.teacherGap);
        calc.resetWorkingSolution(t);
        checkCount("S1 doubled: 93 + 20*3",
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
    addCanonicalConstraints(calc, t.weights);

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
        // The assembly uses the same weights as the table, so the plugin
        // score must equal the monolithic full recompute bit-for-bit.
        const Score recomputed
            = ScoreCalculator::computeBreakdown(t).toScore(t.weights);
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
    d.t.weights = ObjectiveWeights{};  // CP-SAT defaults

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
        addCourse(c, {MATH[c]}, 5, 1, -1, 0, 0x000F, false);  // 오전 선호
        addCourse(c, {ENG[c]}, c == 2 ? 4 : 5, 1, -1, 0, 0, false);
        addCourse(c, {SCI[c]}, 4, 1, /*SCI*/ 0, 0, 0, false);
        addCourse(c, {SOC[c]}, 4, 1, -1, 0, 0, false);
        addCourse(c, {SOC[c]}, 1, 1, -1, 0, 0, false);  // ETH, same teacher
        addCourse(
            c, {10}, 3, 1, /*GYM*/ 3, 0x0001, 0, false);  // PE, 1교시 기피
        addCourse(c, {11}, 2, 1, /*MUS*/ 1, 0, 0, false);
        d.artBlocks.push_back(
            addCourse(c, {12}, 2, 2, /*ART*/ 2, 0, 0, true));  // 2연강
        if (c == 0)
            d.mergedTech = addCourse(0, {13, 14}, 2, 1, -1, 0, 0, false);
        else
            addCourse(c, {TECH[c]}, 2, 1, -1, 0, 0, false);
        // Homeroom hour (창체): pinned to Monday period 1.
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
    config.weights = ObjectiveWeights{};  // CP-SAT default weights

    DecomposedCalculator calc;
    addCanonicalConstraints(calc, d.t.weights);

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
              && d.t.score.soft
                     == -static_cast<double>(b.softPenalty(d.t.weights)),
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
        t.courseDailyLimit.push_back(5);  // H7 must not be the binding limit
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
    std::printf("== (f) H1 demoted to soft: unassigning becomes a choice ==\n");

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(3);
    config.randomSeed = 1;
    config.weights = kZeroWeights;

    // H1 hard (the default): every lesson takes a slot and the leftovers show
    // up as class conflicts — the search has no way to leave one out.
    Timetable strict = buildOverConstrained();
    DecomposedCalculator hard;
    addCanonicalConstraints(hard, kZeroWeights);
    Solver(config).solve(strict, hard);
    const Breakdown strictBreakdown = ScoreCalculator::computeBreakdown(strict);
    checkCount(
        "H1 hard: nothing left unassigned", strictBreakdown.unassigned, 0);
    check(strictBreakdown.classConflicts > 0,
          "H1 hard: the overflow lands on H3 instead");

    // H1 soft: the 10 lessons that do not fit come out, and what stays is
    // conflict-free — the CP-SAT WeeklyPeriodCounts(hard=false) contract.
    Timetable partial = buildOverConstrained();
    DecomposedCalculator soft;
    assembleExcept(soft, kZeroWeights, "H1 unassigned lesson");
    auto h1 = std::make_unique<UnassignedLesson>(/*weight=*/10000.0);
    h1->isHard = false;  // demote: an unplaced lesson is a price, not a veto
    soft.add(std::move(h1));
    Solver(config).solve(partial, soft);
    const Breakdown breakdown = ScoreCalculator::computeBreakdown(partial);
    checkCount("H1 soft: 30 lessons - 20 slots left unassigned",
               breakdown.unassigned,
               10);
    checkCount("H1 soft: what stayed is conflict-free",
               breakdown.hardTotal() - breakdown.unassigned,
               0);
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
    verifySampleScaleSolve();

    if (failures == 0)
    {
        std::printf("All canonical-plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d canonical-plugin check(s) FAILED.\n", failures);
    return 1;
}

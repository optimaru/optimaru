// School-timetabling unit tests (engine test/ style: check() + non-zero exit on
// any failure):
//   (a) every constraint verified in isolation against hand-computed scores
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's ScoreDirector protocol (before/set/after)
//   (c) a small instance solved under EnvironmentMode::FULL_ASSERT
//   (d) pinned lessons keep their assignment through a solve
//   (e) the two search strategies (best-improvement descent, destroy-repair as
//       an ordinary move) under FULL_ASSERT, alone and together
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "st/domain.hpp"
#include "st/incremental_calculator.hpp"
#include "st/score_calculator.hpp"
#include "st/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace st;

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
                double expectedSoft)
{
    const bool ok = (actual.hard == expectedHard
                     && std::fabs(actual.soft - expectedSoft) < 1e-9);
    std::printf("  [%s] %-40s got=%s expected=%ldhard/%gsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedSoft);
    if (!ok)
        ++failures;
}

// Base problem: `days` x `periodsPerDay` timeslots (index = day * periodsPerDay
// + period - 1) and `roomCount` rooms, no lessons yet.
Timetable makeBase(int days, int periodsPerDay, int roomCount)
{
    Timetable t;
    for (int d = 0; d < days; ++d)
    {
        for (int p = 1; p <= periodsPerDay; ++p)
            t.timeslots.push_back({d, p});
    }
    for (int r = 0; r < roomCount; ++r)
    {
        t.rooms.push_back({"R" + std::to_string(r), "일반"});
    }
    return t;
}

void addLesson(Timetable &t,
               const std::string &subject,
               const std::string &teacher,
               const std::string &group,
               int timeslotIndex = -1,
               int roomIndex = -1)
{
    Lesson lesson;
    lesson.id = "L" + std::to_string(t.lessons.size());
    lesson.subject = subject;
    lesson.teacher = teacher;
    lesson.studentGroup = group;
    lesson.slotRoom
        = timeslotIndex < 0 ? -1 : t.slotRoomOf(timeslotIndex, roomIndex);
    t.lessons.push_back(lesson);
}

// -------------------------------------------------------------------------
// (a) Each constraint in isolation, full recompute vs hand-computed scores.
// Base grid: 2 days x 3 periods (slots 0..5; 0,1,2 = Mon 1-3교시), 3 rooms.
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Verifying each constraint in isolation ==\n");

    // hard 1: roomConflict — same timeslot, same room, otherwise unrelated.
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 0, 0);
        addLesson(t, "영어", "TB", "G2", 0, 0);
        checkScore(
            "roomConflict pair", ScoreCalculator::calculateScore(t), -1, 0);
    }
    // hard 1: three lessons in one cell -> C(3,2) = 3 pairs.
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 0, 0);
        addLesson(t, "영어", "TB", "G2", 0, 0);
        addLesson(t, "과학", "TC", "G3", 0, 0);
        checkScore(
            "roomConflict triple", ScoreCalculator::calculateScore(t), -3, 0);
    }
    // hard 2: teacherConflict — same teacher, same timeslot, different rooms.
    // The pair is also a different-room pair of one teacher: stability -1.
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 0, 0);
        addLesson(t, "영어", "TA", "G2", 0, 1);
        checkScore("teacherConflict (+stability)",
                   ScoreCalculator::calculateScore(t),
                   -1,
                   -1);
    }
    // hard 3: studentGroupConflict — same group, same timeslot, different
    // teachers/rooms/subjects.
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 0, 0);
        addLesson(t, "영어", "TB", "G1", 0, 1);
        checkScore(
            "studentGroupConflict", ScoreCalculator::calculateScore(t), -1, 0);
    }
    // hard 4: teacherUnavailable — one lesson on a blocked timeslot.
    {
        Timetable t = makeBase(2, 3, 3);
        t.teacherUnavailable["TA"] = {0};
        addLesson(t, "수학", "TA", "G1", 0, 0);
        checkScore("teacherUnavailable hit",
                   ScoreCalculator::calculateScore(t),
                   -1,
                   0);
    }
    // hard 4: the same lesson on an open timeslot costs nothing.
    {
        Timetable t = makeBase(2, 3, 3);
        t.teacherUnavailable["TA"] = {0};
        addLesson(t, "수학", "TA", "G1", 3, 0);
        checkScore("teacherUnavailable miss",
                   ScoreCalculator::calculateScore(t),
                   0,
                   0);
    }
    // soft 5: teacherRoomStability — same teacher on different days, different
    // rooms -> -1; same room -> 0.
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 0, 0);
        addLesson(t, "국어", "TA", "G2", 3, 1);
        checkScore("teacherRoomStability differ",
                   ScoreCalculator::calculateScore(t),
                   0,
                   -1);
    }
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 0, 0);
        addLesson(t, "국어", "TA", "G2", 3, 0);
        checkScore("teacherRoomStability same",
                   ScoreCalculator::calculateScore(t),
                   0,
                   0);
    }
    // soft 5: rooms [A, A, B] -> C(3,2) - C(2,2) = 2 different-room pairs.
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 0, 0);
        addLesson(
            t, "국어", "TA", "G2", 2, 0);  // periods 1 and 3: not adjacent
        addLesson(t, "영어", "TA", "G3", 3, 1);
        checkScore("teacherRoomStability [A,A,B]",
                   ScoreCalculator::calculateScore(t),
                   0,
                   -2);
    }
    // soft 6: teacherTimeEfficiency — same teacher, same day, consecutive
    // periods, same room -> +1 reward.
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 0, 0);
        addLesson(t, "국어", "TA", "G2", 1, 0);
        checkScore("teacherTimeEfficiency adjacent",
                   ScoreCalculator::calculateScore(t),
                   0,
                   +1);
    }
    // soft 6: periods 1 and 3 are not consecutive -> no reward.
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 0, 0);
        addLesson(t, "국어", "TA", "G2", 2, 0);
        checkScore("teacherTimeEfficiency gap",
                   ScoreCalculator::calculateScore(t),
                   0,
                   0);
    }
    // soft 6: last period of Mon and first period of Tue are NOT consecutive
    // even though their timeslot indices are adjacent.
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 2, 0);
        addLesson(t, "국어", "TA", "G2", 3, 0);
        checkScore("teacherTimeEfficiency day border",
                   ScoreCalculator::calculateScore(t),
                   0,
                   0);
    }
    // soft 7: studentGroupSubjectVariety — same group+subject on consecutive
    // periods (different teachers, to isolate) -> -1.
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 0, 0);
        addLesson(t, "수학", "TB", "G1", 1, 1);
        checkScore("studentGroupSubjectVariety",
                   ScoreCalculator::calculateScore(t),
                   0,
                   -1);
    }
    // soft 6 + 7 together: the usual same-teacher double period. variety -1,
    // efficiency +1, stability 0 (same room) -> net 0.
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 0, 0);
        addLesson(t, "수학", "TA", "G1", 1, 0);
        checkScore("double period (efficiency+variety)",
                   ScoreCalculator::calculateScore(t),
                   0,
                   0);
    }
    // Unassigned lessons contribute nothing.
    {
        Timetable t = makeBase(2, 3, 3);
        t.teacherUnavailable["TA"] = {0};
        addLesson(t, "수학", "TA", "G1", 0, 0);
        addLesson(t, "영어", "TA", "G1");  // unassigned
        // Only the assigned lesson counts: teacherUnavailable -1, nothing else.
        checkScore("unassigned contributes nothing",
                   ScoreCalculator::calculateScore(t),
                   -1,
                   0);
    }
    // Pinned lessons are scored like any other lesson.
    {
        Timetable t = makeBase(2, 3, 3);
        addLesson(t, "수학", "TA", "G1", 0, 0);
        t.lessons.back().pinned = true;
        addLesson(t, "영어", "TB", "G2", 0, 0);
        checkScore("pinned lesson still scored",
                   ScoreCalculator::calculateScore(t),
                   -1,
                   0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random mid-size instance for the incremental cross-check and the solves.
// -------------------------------------------------------------------------
Timetable buildRandomInstance(int lessonCount, std::uint64_t seed)
{
    Timetable t = makeBase(/*days*/ 3, /*periodsPerDay*/ 4, /*rooms*/ 3);
    const std::vector<std::string> teachers = {"TA", "TB", "TC", "TD"};
    const std::vector<std::string> groups = {"GA", "GB", "GC"};
    const std::vector<std::string> subjects
        = {"국어", "수학", "영어", "과학", "체육"};
    std::mt19937_64 rng(seed);
    for (int i = 0; i < lessonCount; ++i)
    {
        addLesson(t,
                  subjects[rng() % subjects.size()],
                  teachers[rng() % teachers.size()],
                  groups[rng() % groups.size()]);
    }
    t.teacherUnavailable["TA"] = {0, 5};
    t.teacherUnavailable["TC"] = {7};
    return t;
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves (including unassignments), driven through the ScoreDirector so
// the before/set/after hook contract is exercised exactly like in the engine.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Verifying incremental scoring == full recompute ==\n");
    Timetable t = buildRandomInstance(/*lessonCount*/ 24, /*seed*/ 7);

    SchoolTimetablingIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<Timetable> director(t, calc);
    Timetable shadow = t;

    std::mt19937_64 rng(123);
    const int lessonCount = static_cast<int>(t.lessons.size());
    const int valueCount = t.slotRoomCount();
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
            // SwapMove.
            const int a = static_cast<int>(rng() % lessonCount);
            const int b = static_cast<int>(rng() % lessonCount);
            const int va = t.lessons[a].slotRoom;
            const int vb = t.lessons[b].slotRoom;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < lessonCount; ++i)
        {
            shadow.lessons[i].slotRoom = t.lessons[i].slotRoom;
        }
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
// (c) FULL_ASSERT solve: the engine cross-checks the incremental score
// against a full reset + recompute after every single move and throws
// ScoreCorruptionError on any divergence.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (c) FULL_ASSERT solve ==\n");
    Timetable t = buildRandomInstance(/*lessonCount*/ 18, /*seed*/ 11);

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
        check(final.hard == recomputed.hard
                  && std::fabs(final.soft - recomputed.soft) < 1e-9,
              "solver-reported score == full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Pinned lessons keep their assignment through a solve, and every lesson
// ends up assigned.
// -------------------------------------------------------------------------
void verifyPinnedSolve()
{
    std::printf("== (d) Pinned lesson survives a solve ==\n");
    Timetable t = buildRandomInstance(/*lessonCount*/ 10, /*seed*/ 21);
    const int pinnedValue = t.slotRoomOf(/*timeslot*/ 4, /*room*/ 2);
    t.lessons[0].slotRoom = pinnedValue;
    t.lessons[0].pinned = true;

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(1);
    config.randomSeed = 5;
    Solver solver(config);
    solver.solve(t);

    check(t.lessons[0].slotRoom == pinnedValue,
          "pinned lesson kept its slotRoom");
    bool allAssigned = true;
    for (const Lesson &lesson : t.lessons)
    {
        if (lesson.slotRoom < 0)
            allAssigned = false;
    }
    check(allAssigned, "every lesson assigned after the solve");
    check(t.score.hard == 0, "small instance reaches 0hard");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) The two local search strategies,
// searches: best-improvement descent over a nearby lesson pair, and
// destroy-and-repair as an ordinary acceptor-judged move (rather than only a
// stagnation kick). Both mutate through multi-step sequences whose rollback
// is where a bug would hide — a descent sweep restores each trial value
// before committing the best one, and a rejected destroy-repair is rolled
// back from a whole-solution snapshot. A pinned lesson is in the instance so
// the pinned guards on both paths are exercised too.
// -------------------------------------------------------------------------
void verifySearchStrategies()
{
    std::printf("== (e) Descent + destroy-repair-as-move (FULL_ASSERT) ==\n");
    struct Case
    {
        const char *label;
        double descent;
        double drMove;
    };
    const Case cases[] = {
        {"descent only", 0.05, 0.0},
        {"destroy-repair as move only", 0.0, 0.15},
        {"both", 0.05, 0.15},
        {"both, heavily weighted", 0.30, 0.40},
    };
    for (const Case &testCase : cases)
    {
        Timetable t = buildRandomInstance(/*lessonCount*/ 18, /*seed*/ 11);
        const int pinnedValue = t.slotRoomOf(/*timeslot*/ 3, /*room*/ 1);
        t.lessons[0].slotRoom = pinnedValue;
        t.lessons[0].pinned = true;

        Solver::Config config;
        config.spentLimit = std::chrono::seconds(2);
        config.randomSeed = 3;
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        config.descentWeight = testCase.descent;
        config.ruinRecreateAsMoveWeight = testCase.drMove;
        config.ruinRecreateUnimprovedStepTrigger = 200;
        config.ruinRecreatePolishSteps = 100;

        bool corrupted = false;
        try
        {
            Solver(config).solve(t);
        }
        catch (const optimaru::ScoreCorruptionError &e)
        {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }

        bool allAssigned = true;
        for (const Lesson &lesson : t.lessons)
        {
            if (lesson.slotRoom < 0)
                allAssigned = false;
        }
        const Score recomputed = ScoreCalculator::calculateScore(t);
        const bool matches
            = t.score.hard == recomputed.hard
              && std::fabs(t.score.soft - recomputed.soft) < 1e-9;
        check(!corrupted && allAssigned && matches
                  && t.lessons[0].slotRoom == pinnedValue,
              testCase.label);
    }
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifyPinnedSolve();
    verifySearchStrategies();

    if (failures == 0)
    {
        std::printf("All school-timetabling checks PASSED.\n");
        return 0;
    }
    std::printf("%d school-timetabling check(s) FAILED.\n", failures);
    return 1;
}

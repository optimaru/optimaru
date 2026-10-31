// Canonical-domain (st::canon) unit tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) every hard (H1, H3~H10) and soft (S1~S6) constraint verified in
//       isolation against hand-computed counts, several reproducing the
//       CP-SAT package's test cases (benchmarks/school-timetabling/cpsat/tests)
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's ScoreDirector protocol (before/set/after)
//   (c) a mid-size instance solved under EnvironmentMode::FULL_ASSERT
//   (d) block / merged-lesson / team-teaching solves: hard 0 plus the
//       structural property each feature promises
//   (e) a CP-SAT-sample-scale school (6 classes, 15 teachers, 34 slots)
//       reaches hard 0 within 20 seconds
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
    std::printf("  [%s] %-44s got=%lld expected=%lld\n",
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

// -------------------------------------------------------------------------
// (a) Hard constraints in isolation. Grid: 2 days x 5 periods (slots 0..4 =
// Mon periods 1-5, 5..9 = Tue). Weights are zero, so soft must stay 0.
// -------------------------------------------------------------------------
const std::vector<int> kGrid25{5, 5};

void verifyHardConstraints()
{
    std::printf("== (a1) Hard constraints, hand-computed ==\n");

    // H1: unassigned lessons cost 1 hard each.
    {
        Timetable t = makeGrid(kGrid25);
        addLesson(t, 0, 0, {0});
        addLesson(t, 0, 1, {1});
        addLesson(t, 0, 2, {2}, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("H1 two unassigned lessons", b.unassigned, 2);
        checkCount("H1 hard total", b.hardTotal(), 2);
    }
    // H3: class conflict — same class, same slot, C(n,2).
    {
        Timetable t = makeGrid(kGrid25);
        addLesson(t, 0, 0, {0}, 0);
        addLesson(t, 0, 1, {1}, 0);
        Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("H3 class conflict pair", b.classConflicts, 1);
        addLesson(t, 0, 2, {2}, 0);
        t.prepared = false;  // structural change: re-run prepare()
        b = ScoreCalculator::computeBreakdown(t);
        checkCount("H3 class conflict triple = C(3,2)", b.classConflicts, 3);
    }
    // H4: teacher conflict — same teacher, same slot, different classes.
    {
        Timetable t = makeGrid(kGrid25);
        addLesson(t, 0, 0, {0}, 0);
        addLesson(t, 1, 1, {0}, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("H4 teacher conflict pair", b.teacherConflicts, 1);
        checkCount("H4 no class conflict across classes", b.classConflicts, 0);
    }
    // H4: team teaching — two lessons sharing BOTH teachers conflict once
    // per teacher.
    {
        Timetable t = makeGrid(kGrid25);
        addLesson(t, 0, 0, {0, 1}, 0);
        addLesson(t, 1, 1, {0, 1}, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("H4 team-teaching double conflict", b.teacherConflicts, 2);
    }
    // H5: assignment outside the allowedSlot table.
    {
        Timetable t = makeGrid(kGrid25);
        const int i = addLesson(t, 0, 0, {0}, 0);
        t.lessons[i].allowedSlot.assign(10, 1);
        t.lessons[i].allowedSlot[0] = 0;
        Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("H5 forbidden slot hit", b.slotNotAllowed, 1);
        t.lessons[i].slot = 1;
        b = ScoreCalculator::computeBreakdown(t);
        checkCount("H5 allowed slot ok", b.slotNotAllowed, 0);
    }
    // H6: room-type demand over capacity (cpsat test_room_capacity_respected:
    // one science lab shared by two classes).
    {
        Timetable t = makeGrid(kGrid25);
        t.roomCapacityByType = {1};
        const int a = addLesson(t, 0, 0, {0}, 0);
        const int b1 = addLesson(t, 1, 1, {1}, 0);
        t.lessons[a].roomTypeDemand = {1};
        t.lessons[b1].roomTypeDemand = {1};
        Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("H6 demand 2 > capacity 1", b.roomOverCapacity, 1);
        t.lessons[b1].slot = 1;
        b = ScoreCalculator::computeBreakdown(t);
        checkCount("H6 spread over slots ok", b.roomOverCapacity, 0);
    }
    // H6: a merged lesson carries the SUM of its members' demands.
    {
        Timetable t = makeGrid(kGrid25);
        t.roomCapacityByType = {1};
        const int i = addLesson(t, 0, 0, {0, 1}, 0);
        t.lessons[i].roomTypeDemand = {2};
        Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("H6 merged demand 2 > capacity 1", b.roomOverCapacity, 1);
        t.roomCapacityByType = {2};
        t.prepared = false;
        b = ScoreCalculator::computeBreakdown(t);
        checkCount("H6 merged demand 2 <= capacity 2", b.roomOverCapacity, 0);
    }
    // H7: per-course daily cap (cpsat test_daily_course_limit).
    {
        Timetable t = makeGrid(kGrid25);
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 1));
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 3));
        t.courseDailyLimit = {1};
        Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount(
            "H7 two same-course lessons one day", b.courseDailyExcess, 1);
        t.lessons[1].slot = slotIdx(kGrid25, 1, 3);
        b = ScoreCalculator::computeBreakdown(t);
        checkCount("H7 spread over days ok", b.courseDailyExcess, 0);
    }
    // H8: block groups — violation = (k-1) - same-day consecutive
    // adjacencies.
    {
        auto blockCase = [](const std::vector<std::pair<int, int>> &dayPeriods,
                            std::int64_t expected,
                            const std::string &label)
        {
            Timetable t = makeGrid(kGrid25);
            for (const auto &[day, period] : dayPeriods)
            {
                const int i
                    = addLesson(t, 0, 0, {0}, slotIdx(kGrid25, day, period));
                t.lessons[i].blockGroup = 0;
            }
            t.courseDailyLimit = {4};  // keep H7 out of the way
            const Breakdown b = ScoreCalculator::computeBreakdown(t);
            checkCount(label, b.blockBreaks, expected);
        };
        blockCase({{0, 1}, {0, 2}}, 0, "H8 pair adjacent");
        blockCase({{0, 1}, {0, 3}}, 1, "H8 pair with a hole");
        blockCase({{0, 1}, {1, 1}}, 1, "H8 pair split across days");
        blockCase({{0, 1}, {0, 2}, {0, 3}}, 0, "H8 triple run");
        blockCase({{0, 1}, {0, 2}, {0, 4}}, 1, "H8 triple, one detached");
        blockCase({{0, 1}, {0, 3}, {1, 1}}, 2, "H8 triple fully scattered");
    }
    // H8 vs partial assignment. CP-SAT's _check_blocks asks whether SOME day
    // holds a run of block_periods, so half of a 2-period block is "no 2-period block" there
    // — half a block is charged here too, or partial solving could buy its
    // way out of a block by dropping one member. Dropping the WHOLE group is
    // the deliberate exception: H1 is what prices that.
    {
        Timetable t = makeGrid(kGrid25);
        const int a = addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 1));
        const int b1 = addLesson(t, 0, 0, {0});
        t.lessons[a].blockGroup = 0;
        t.lessons[b1].blockGroup = 0;
        t.courseDailyLimit = {4};
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("H8 half-placed block is charged", b.blockBreaks, 1);
        checkCount("H8 ... and H1 charges the missing hour", b.unassigned, 1);
    }
    {
        Timetable t = makeGrid(kGrid25);
        const int a = addLesson(t, 0, 0, {0});
        const int b1 = addLesson(t, 0, 0, {0});
        t.lessons[a].blockGroup = 0;
        t.lessons[b1].blockGroup = 0;
        t.courseDailyLimit = {4};
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("H8 group dropped whole is free", b.blockBreaks, 0);
        checkCount("H8 ... H1 alone prices it", b.unassigned, 2);
    }
    // H9 daily: distinct busy periods over maxPerDay.
    {
        Timetable t = makeGrid(kGrid25);
        t.teacherMaxPerDay = {2};
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 1));
        addLesson(t, 1, 1, {0}, slotIdx(kGrid25, 0, 3));
        addLesson(t, 2, 2, {0}, slotIdx(kGrid25, 0, 5));
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("H9 3 periods > maxPerDay 2", b.teacherDailyExcess, 1);
    }
    // H9 daily counts DISTINCT periods (validate.py uses a set): a same-slot
    // overlap is an H4 conflict, not extra daily load.
    {
        Timetable t = makeGrid(kGrid25);
        t.teacherMaxPerDay = {1};
        addLesson(t, 0, 0, {0}, 0);
        addLesson(t, 1, 1, {0}, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("H9 same-slot overlap: conflict", b.teacherConflicts, 1);
        checkCount(
            "H9 same-slot overlap: no daily excess", b.teacherDailyExcess, 0);
    }
    // H9 weekly (cpsat test_teacher_weekly_max shape).
    {
        Timetable t = makeGrid(kGrid25);
        t.teacherMaxPerWeek = {2};
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 1));
        addLesson(t, 1, 1, {0}, slotIdx(kGrid25, 0, 3));
        addLesson(t, 2, 2, {0}, slotIdx(kGrid25, 1, 1));
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount(
            "H9 3 weekly periods > maxPerWeek 2", b.teacherWeeklyExcess, 1);
    }
    // H10: consecutive-run excess (cpsat test_teacher_consecutive_limit).
    {
        auto runCase = [](std::vector<int> periods,
                          int limit,
                          std::int64_t expected,
                          const std::string &label)
        {
            Timetable t = makeGrid(kGrid25);
            t.teacherMaxConsecutive = {limit};
            t.teacherMaxPerDay = {6};
            int next = 0;
            for (int p : periods)
            {
                const int id = next++;
                addLesson(t, id, id, {0}, slotIdx(kGrid25, 0, p));
            }
            const Breakdown b = ScoreCalculator::computeBreakdown(t);
            checkCount(label, b.consecutiveExcess, expected);
        };
        runCase({1, 2, 3}, 2, 1, "H10 run 3 over limit 2");
        runCase({1, 2, 3, 4, 5}, 2, 3, "H10 run 5 over limit 2");
        runCase({1, 2, 4, 5}, 2, 0, "H10 two runs of 2 ok");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (a2) Soft constraints: validate.py's compute_penalties, key for key.
// -------------------------------------------------------------------------
void verifySoftConstraints()
{
    std::printf("== (a2) Soft constraints, hand-computed ==\n");

    // S1: teacher gap = last - first + 1 - count of the day's busy periods.
    {
        Timetable t = makeGrid(kGrid25);
        t.weights.teacherGap = 1;
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 1));
        addLesson(t, 1, 1, {0}, slotIdx(kGrid25, 0, 3));
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("S1 periods {1,3} -> gap 1", b.teacherGap, 1);
        checkCount("S1 weighted soft penalty", b.softPenalty(t.weights), 1);
    }
    // S1: an unavailable period wedged between lessons still counts as a gap
    // (validate.py counts busy periods only, ignoring availability).
    {
        Timetable t = makeGrid(kGrid25);
        const int a = addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 1));
        const int b1 = addLesson(t, 1, 1, {0}, slotIdx(kGrid25, 0, 3));
        for (int i : {a, b1})
        {
            t.lessons[i].allowedSlot.assign(10, 1);
            t.lessons[i].allowedSlot[slotIdx(kGrid25, 0, 2)] = 0;
        }
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("S1 unavailable period still a gap", b.teacherGap, 1);
    }
    // S1+S4: cpsat test_forced_gap_matches_hand_count — one class, periods 1
    // and 5 fixed -> 3 gaps for both the teacher and the class.
    {
        Timetable t = makeGrid(kGrid25);
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 1));
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 5));
        t.courseDailyLimit = {2};
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("S1 forced gap: teacher 3", b.teacherGap, 3);
        checkCount("S4 forced gap: class 3", b.classGap, 3);
        checkCount("hard stays 0", b.hardTotal(), 0);
    }
    // S2: one workday per (teacher, day) with any lesson.
    {
        Timetable t = makeGrid(kGrid25);
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 1));
        addLesson(t, 1, 1, {0}, slotIdx(kGrid25, 0, 2));
        addLesson(t, 2, 2, {0}, slotIdx(kGrid25, 1, 1));
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("S2 lessons on two days -> 2 workdays", b.teacherWorkday, 2);
    }
    // S3: workday on a preferred-free day (cpsat
    // test_forced_free_day_violation_counted).
    {
        Timetable t = makeGrid(kGrid25);
        t.teacherPreferredFreeDays = {1 << 1};  // Tue
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 1, 1));
        Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("S3 lesson on preferred-free day", b.teacherFreeDay, 1);
        t.lessons[0].slot = slotIdx(kGrid25, 0, 1);
        b = ScoreCalculator::computeBreakdown(t);
        checkCount("S3 lesson moved off the free day", b.teacherFreeDay, 0);
    }
    // S4: class gap with different teachers (isolated from S1 by weights).
    {
        Timetable t = makeGrid(kGrid25);
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 2));
        addLesson(t, 0, 1, {1}, slotIdx(kGrid25, 0, 4));
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("S4 class periods {2,4} -> gap 1", b.classGap, 1);
        checkCount("S1 single-lesson teachers have no gap", b.teacherGap, 0);
    }
    // S5/S6: cpsat test_period_preferences_steer_placement — avoided {1},
    // preferred {2}, lesson forced to period 1 hits both.
    {
        Timetable t = makeGrid(kGrid25);
        t.courseAvoidedPeriodMask = {0x0001};    // period 1
        t.coursePreferredPeriodMask = {0x0002};  // period 2
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 1));
        Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("S5 avoided period hit", b.avoidedPeriod, 1);
        checkCount("S6 off the preferred periods", b.nonPreferredPeriod, 1);
        t.lessons[0].slot = slotIdx(kGrid25, 0, 2);
        b = ScoreCalculator::computeBreakdown(t);
        checkCount("S5 clean period", b.avoidedPeriod, 0);
        checkCount("S6 preferred period", b.nonPreferredPeriod, 0);
    }
    // S6: an empty preferred mask means no preference.
    {
        Timetable t = makeGrid(kGrid25);
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 5));
        const Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("S6 no preference, no penalty", b.nonPreferredPeriod, 0);
    }
    // S5/S6 preference units: a merged lesson carries one unit per member
    // course and each unit counts on its own (validate.py counts the member
    // lessons individually). Units {avoided {1}}, {avoided {1}, preferred
    // {2}} at period 1: S5 = 2, S6 = 1; moved to period 2 both clear.
    {
        Timetable t = makeGrid(kGrid25);
        const int i = addLesson(t, 0, 0, {0, 1}, slotIdx(kGrid25, 0, 1));
        t.lessons[i].preferenceUnits = {{0x0001, 0x0000}, {0x0001, 0x0002}};
        Breakdown b = ScoreCalculator::computeBreakdown(t);
        checkCount("S5 merged units, both avoided", b.avoidedPeriod, 2);
        checkCount("S6 merged units, one preferred", b.nonPreferredPeriod, 1);
        t.lessons[i].slot = slotIdx(kGrid25, 0, 2);
        b = ScoreCalculator::computeBreakdown(t);
        checkCount("S5 merged units clean period", b.avoidedPeriod, 0);
        checkCount("S6 merged units on preferred", b.nonPreferredPeriod, 0);
    }
    // Weighted assembly: soft score = -(weighted sum) with the CP-SAT default
    // weights.
    {
        Timetable t = makeGrid(kGrid25);
        t.weights = ObjectiveWeights{};  // 20/3/30/10/5/1
        t.teacherPreferredFreeDays = {1 << 0};
        t.courseAvoidedPeriodMask = {0x0001, 0};
        addLesson(t, 0, 0, {0}, slotIdx(kGrid25, 0, 1));
        addLesson(t, 0, 1, {0}, slotIdx(kGrid25, 0, 3));
        // teacherGap 1, workday 1, freeDay 1, classGap 1, avoided 1: soft =
        // -(20 + 3 + 30 + 10 + 5) = -68.
        const Score score = ScoreCalculator::calculateScore(t);
        checkCount("weighted soft assembly",
                   static_cast<std::int64_t>(-score.soft),
                   68);
        checkCount(
            "weighted hard stays 0", static_cast<std::int64_t>(-score.hard), 0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random feature-complete instance for the incremental cross-check and the
// FULL_ASSERT solve: team teaching, blocks, room demand, period masks,
// tight teacher limits, all soft weights on.
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
        // Guarantee team teaching and block coverage (courses 0..2), plus a
        // random sprinkle of both.
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
// (b) Incremental score == full recompute across a random walk of change and
// swap moves (including unassignments), driven through the ScoreDirector so
// the before/set/after hook contract is exercised exactly like in the engine.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Incremental scoring == full recompute ==\n");
    Timetable t = buildRandomInstance(/*courseCount*/ 16, /*seed*/ 7);
    ScoreCalculator::prepare(t);
    std::printf("  instance: %d lessons, %d slots, %d block groups\n",
                static_cast<int>(t.lessons.size()),
                t.slotCount(),
                t.blockGroupCount);

    CanonicalIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<Timetable> director(t, calc);
    Timetable shadow = t;

    std::mt19937_64 rng(123);
    const int lessonCount = static_cast<int>(t.lessons.size());
    const int valueCount = t.slotCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    int hotMismatches = 0;
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
        if (!calc.hotSetsConsistent())
            ++hotMismatches;
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
    check(hotMismatches == 0,
          "hot violation sets match the occupancy counters at every step");
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
    Timetable t = buildRandomInstance(/*courseCount*/ 8, /*seed*/ 11);

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
// (d) Feature scenarios: solve, reach hard 0, verify the structure the
// feature promises.
// -------------------------------------------------------------------------
Solver::Config quickSolveConfig(std::uint64_t seed)
{
    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.bestScoreLimit = Score{0, -std::numeric_limits<double>::infinity()};
    config.randomSeed = seed;
    config.weights = kZeroWeights;  // pure satisfaction, like cpsat's tests
    return config;
}

void verifyBlockScenario()
{
    std::printf("== (d1) Block (consecutive periods) scenario ==\n");
    const std::vector<int> ppd{4, 4};
    Timetable t = makeGrid(ppd);
    // Course 0: a 2-period block; courses 1..3 one filler each; course 4 two
    // lessons capped at 1/day. 7 lessons in 8 class slots.
    t.courseDailyLimit = {2, 1, 1, 1, 1};
    const int blockA = addLesson(t, 0, 0, {0});
    const int blockB = addLesson(t, 0, 0, {0});
    t.lessons[blockA].blockGroup = 0;
    t.lessons[blockB].blockGroup = 0;
    for (int c = 1; c <= 3; ++c)
        addLesson(t, 0, c, {1});
    addLesson(t, 0, 4, {2});
    addLesson(t, 0, 4, {2});

    Solver solver(quickSolveConfig(1));
    solver.solve(t);
    const Breakdown b = ScoreCalculator::computeBreakdown(t);
    checkCount("block scenario reaches hard 0", b.hardTotal(), 0);
    const int sa = t.lessons[blockA].slot;
    const int sb = t.lessons[blockB].slot;
    check(sa >= 0 && sb >= 0 && t.dayOfSlot[sa] == t.dayOfSlot[sb]
              && std::abs(t.periodOfSlot[sa] - t.periodOfSlot[sb]) == 1,
          "block lessons sit on one day in consecutive periods");
    std::printf("\n");
}

void verifyMergedLessonScenario()
{
    std::printf("== (d2) Merged-lesson (parallel groups) scenario ==\n");
    const std::vector<int> ppd{3, 3};
    Timetable t = makeGrid(ppd);
    t.roomCapacityByType = {1};
    // Course 0: the merged parallel group — teachers {0, 1} union, summed
    // room demand, one class-occupancy unit, 1/day so its two weekly periods
    // land on different days.
    t.courseDailyLimit = {1, 2, 2};
    std::vector<int> merged;
    for (int i = 0; i < 2; ++i)
    {
        const int lesson = addLesson(t, 0, 0, {0, 1});
        t.lessons[lesson].roomTypeDemand = {1};
        merged.push_back(lesson);
    }
    // Both member teachers also teach plain courses in other classes.
    addLesson(t, 1, 1, {0});
    addLesson(t, 1, 1, {0});
    addLesson(t, 2, 2, {1});
    addLesson(t, 2, 2, {1});

    Solver solver(quickSolveConfig(2));
    solver.solve(t);
    const Breakdown b = ScoreCalculator::computeBreakdown(t);
    checkCount("merged scenario reaches hard 0", b.hardTotal(), 0);
    check(t.dayOfSlot[t.lessons[merged[0]].slot]
              != t.dayOfSlot[t.lessons[merged[1]].slot],
          "merged course spread over two days (daily cap 1)");
    for (int teacher : {0, 1})
    {
        std::set<int> slots;
        int count = 0;
        for (const Lesson &lesson : t.lessons)
        {
            for (int tt : lesson.teachers)
            {
                if (tt == teacher)
                {
                    slots.insert(lesson.slot);
                    ++count;
                }
            }
        }
        check(static_cast<int>(slots.size()) == count,
              "teacher " + std::to_string(teacher)
                  + " has no overlap with the merged lessons");
    }
    std::printf("\n");
}

void verifyTeamTeachingScenario()
{
    std::printf("== (d3) Team-teaching scenario ==\n");
    const std::vector<int> ppd{5};
    Timetable t = makeGrid(ppd);
    // Course 0 is one team-taught lesson; each teacher also has two solo
    // lessons in other classes. 5 periods, teacher loads 3 + 3.
    t.courseDailyLimit = {1, 2, 2};
    const int team = addLesson(t, 0, 0, {0, 1});
    addLesson(t, 1, 1, {0});
    addLesson(t, 1, 1, {0});
    addLesson(t, 2, 2, {1});
    addLesson(t, 2, 2, {1});

    Solver solver(quickSolveConfig(3));
    solver.solve(t);
    const Breakdown b = ScoreCalculator::computeBreakdown(t);
    checkCount("team-teaching scenario reaches hard 0", b.hardTotal(), 0);
    const int teamSlot = t.lessons[team].slot;
    bool overlap = false;
    for (int i = 1; i < static_cast<int>(t.lessons.size()); ++i)
        overlap = overlap || t.lessons[i].slot == teamSlot;
    check(teamSlot >= 0 && !overlap,
          "no other lesson of either teacher shares the team slot");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) CP-SAT-sample-scale demo school (teacher_scheduling/sample.py rebuilt
// in C++): 5 days (7/7/7/7/6 periods = 34 slots), 6 classes x 34 weekly
// periods (a completely dense timetable), 15 teachers, special rooms
// (2 science labs, 1 music room, 1 art room, 1 gym), an art double period
// per class, the merged C11 tech/home-economics parallel group, one
// team-taught English hour and a pinned homeroom hour on Monday period 1.
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
        addCourse(c, {MATH[c]}, 5, 1, -1, 0, 0x000F, false);  // prefers mornings
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
    std::printf("== (e) Sample-scale school reaches hard 0 in 20s ==\n");
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
    config.parallelSolverCount = 4;
    config.weights = ObjectiveWeights{};  // CP-SAT default weights

    const auto start = std::chrono::steady_clock::now();
    Solver solver(config);
    solver.solve(d.t);
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - start)
                               .count();

    const Breakdown b = ScoreCalculator::computeBreakdown(d.t);
    std::printf(
        "  solved in %.1fs, score %s\n", seconds, d.t.score.toString().c_str());
    checkCount("sample-scale hard total", b.hardTotal(), 0);
    check(seconds <= 20.5, "finished within the 20s budget");

    bool allAssigned = true;
    for (const Lesson &lesson : d.t.lessons)
        allAssigned = allAssigned && lesson.slot >= 0;
    check(allAssigned, "every lesson assigned");

    bool hrPinned = true;
    for (int i : d.hrLessons)
        hrPinned = hrPinned && d.t.lessons[i].slot == 0;
    check(hrPinned, "homeroom hours stayed pinned to Monday period 1");

    bool artOk = true;
    for (const std::vector<int> &pair : d.artBlocks)
    {
        const int sa = d.t.lessons[pair[0]].slot;
        const int sb = d.t.lessons[pair[1]].slot;
        artOk = artOk && d.t.dayOfSlot[sa] == d.t.dayOfSlot[sb]
                && std::abs(d.t.periodOfSlot[sa] - d.t.periodOfSlot[sb]) == 1;
    }
    check(artOk, "every art double period is consecutive on one day");

    check(d.t.dayOfSlot[d.t.lessons[d.mergedTech[0]].slot]
              != d.t.dayOfSlot[d.t.lessons[d.mergedTech[1]].slot],
          "merged tech group spread over two days");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyHardConstraints();
    verifySoftConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifyBlockScenario();
    verifyMergedLessonScenario();
    verifyTeamTeachingScenario();
    verifySampleScaleSolve();

    if (failures == 0)
    {
        std::printf("All canonical-domain checks PASSED.\n");
        return 0;
    }
    std::printf("%d canonical-domain check(s) FAILED.\n", failures);
    return 1;
}

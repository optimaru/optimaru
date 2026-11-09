// Meeting-scheduling unit tests (engine test/ style: check() + non-zero exit on
// any failure):
//   (a) every constraint verified in isolation against hand-computed counts
//       (the Java MeetingSchedulingConstraintProviderTest cases, ported to
//       the Breakdown of the full recompute)
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's Editor protocol (before/set/after)
//   (c) a small instance solved under ScoreChecks::ScoreChecks::Full
//   (d) pinned assignments keep their value through a solve; the demo data
//       reaches hard 0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "ms/demo_data.hpp"
#include "ms/domain.hpp"
#include "ms/incremental_calculator.hpp"
#include "ms/score_calculator.hpp"
#include "ms/solver.hpp"
#include "optimaru/model/editor.hpp"

using namespace ms;

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
                std::int64_t actual,
                std::int64_t expected)
{
    const bool ok = actual == expected;
    std::printf("  [%s] %-46s got=%lld expected=%lld\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                static_cast<long long>(actual),
                static_cast<long long>(expected));
    if (!ok)
        ++failures;
}

// Base problem: `days` x `grainsPerDay` time grains (grainIndex 0-based
// contiguous like the Java tests, dayOfYear 100+day, 08:00 + 15min steps)
// and one room per given capacity. No meetings yet.
MeetingSchedule makeBase(int days,
                         int grainsPerDay,
                         const std::vector<int> &roomCapacities,
                         int personCount = 4)
{
    MeetingSchedule s;
    for (int p = 0; p < personCount; ++p)
        s.people.push_back({"P" + std::to_string(p), "Person"});
    int index = 0;
    for (int d = 0; d < days; ++d)
    {
        for (int g = 0; g < grainsPerDay; ++g)
        {
            s.timeGrains.push_back(
                {std::to_string(index), index, 100 + d, 8 * 60 + 15 * g});
            ++index;
        }
    }
    for (std::size_t r = 0; r < roomCapacities.size(); ++r)
    {
        s.rooms.push_back({"R" + std::to_string(r), "Room", roomCapacities[r]});
    }
    return s;
}

// Adds a meeting plus its 1:1 assignment; grainPos < 0 leaves it unassigned.
void addMeeting(MeetingSchedule &s,
                int durationInGrains,
                const std::vector<int> &required,
                const std::vector<int> &preferred,
                int grainPos = -1,
                int roomIndex = 0)
{
    Meeting meeting;
    meeting.id = std::to_string(s.meetings.size());
    meeting.topic = "M" + meeting.id;
    meeting.durationInGrains = durationInGrains;
    meeting.requiredAttendees = required;
    meeting.preferredAttendees = preferred;
    s.meetings.push_back(meeting);

    MeetingAssignment assignment;
    assignment.id = meeting.id;
    assignment.meeting = static_cast<int>(s.meetings.size()) - 1;
    assignment.grainRoom
        = grainPos < 0 ? -1 : s.grainRoomOf(grainPos, roomIndex);
    s.meetingAssignments.push_back(assignment);
}

// -------------------------------------------------------------------------
// (a) Each constraint in isolation (the Java ConstraintVerifier cases).
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Verifying each constraint in isolation ==\n");

    // hard 1: roomConflictCost — same room, back to back: no overlap.
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 4, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("roomConflictCost unpenalized", b.roomConflictCost, 0);
        // The pair is also back-to-back: one consecutive-meetings pair.
        checkCount(
            "  ... consecutive pair counted", b.consecutiveMeetingsCost, 1);
    }
    // hard 1: overlap of 2 grains in the same room (Java: penalizesBy 2).
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 2, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("roomConflictCost penalized", b.roomConflictCost, 2);
        // Any-meeting overlap is penalized too (soft 10).
        checkCount("  ... overlapping grains", b.meetingOverlapCost, 2);
    }
    // hard 1: different rooms — no room conflict, still overlapping.
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 2, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("roomConflictCost different rooms", b.roomConflictCost, 0);
        checkCount("  ... overlapping grains", b.meetingOverlapCost, 2);
    }
    // hard 2: overtimeCost — the last grain exists: no penalty.
    {
        MeetingSchedule s = makeBase(1, 4, {5});
        addMeeting(s, 4, {}, {}, 0, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("overtimeCost unpenalized", b.overtimeCost, 0);
    }
    // hard 2: last grain index 5 does not exist (grains 0..3): penalty 5
    // (Java penalizes by the lastTimeGrainIndex itself).
    {
        MeetingSchedule s = makeBase(1, 4, {5});
        addMeeting(s, 4, {}, {}, 2, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("overtimeCost penalized", b.overtimeCost, 5);
    }
    // hard 3: requiredAttendanceConflictCost — shared person, overlap 2.
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {0}, {}, 0, 0);
        addMeeting(s, 4, {0}, {}, 2, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("requiredAttendanceConflictCost penalized",
                   b.requiredAttendanceConflictCost,
                   2);
    }
    // hard 3: back to back — no conflict.
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {0}, {}, 0, 0);
        addMeeting(s, 4, {0}, {}, 4, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("requiredAttendanceConflictCost unpenalized",
                   b.requiredAttendanceConflictCost,
                   0);
    }
    // hard 3: two shared required people double the weight.
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {0, 1}, {}, 0, 0);
        addMeeting(s, 4, {0, 1}, {}, 3, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("requiredAttendanceConflictCost two people",
                   b.requiredAttendanceConflictCost,
                   2);
    }
    // hard 4: roomCapacityCost — 1 required + 1 preferred vs capacity 2
    // (Java: unpenalized) and capacity 1 (Java: penalizes by 1).
    {
        MeetingSchedule s = makeBase(1, 12, {2});
        addMeeting(s, 4, {0}, {1}, 0, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("roomCapacityCost unpenalized", b.roomCapacityCost, 0);
    }
    {
        MeetingSchedule s = makeBase(1, 12, {1});
        addMeeting(s, 4, {0}, {1}, 0, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("roomCapacityCost penalized", b.roomCapacityCost, 1);
    }
    // hard 5: dayCrossingCost — 2 days x 4 grains; a meeting starting
    // at grain 2 (day 100) ends at grain 5 (day 101).
    {
        MeetingSchedule s = makeBase(2, 4, {5});
        addMeeting(s, 4, {}, {}, 0, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("dayCrossingCost unpenalized", b.dayCrossingCost, 0);
    }
    {
        MeetingSchedule s = makeBase(2, 4, {5});
        addMeeting(s, 4, {}, {}, 2, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("dayCrossingCost penalized", b.dayCrossingCost, 1);
        checkCount("  ... not overtime (grain exists)", b.overtimeCost, 0);
    }
    // medium 6: requiredAndPreferredAttendanceConflictCost — required in one
    // meeting, preferred in another, full overlap of 4 (Java: 4).
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {0}, {}, 0, 0);
        addMeeting(s, 4, {}, {0}, 0, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("requiredAndPreferredAttendanceConflictCost",
                   b.requiredAndPreferredAttendanceConflictCost,
                   4);
        checkCount("  ... no required-required conflict",
                   b.requiredAttendanceConflictCost,
                   0);
    }
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {0}, {}, 0, 0);
        addMeeting(s, 4, {}, {0}, 4, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("requiredAndPreferredAttendanceConflictCost unpenalized",
                   b.requiredAndPreferredAttendanceConflictCost,
                   0);
    }
    // medium 7: preferredAttendanceConflictCost — shared preferred person, full
    // overlap of 4 (Java: 4).
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {0}, 0, 0);
        addMeeting(s, 4, {}, {0}, 0, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("preferredAttendanceConflictCost",
                   b.preferredAttendanceConflictCost,
                   4);
    }
    // soft 8: meetingEndTimeCost — duration 1 at grain 0: last
    // index 0 (Java: 0); duration 4 at grain 0: last index 3 (Java: 3).
    {
        MeetingSchedule s = makeBase(1, 12, {5});
        addMeeting(s, 1, {}, {}, 0, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("meetingEndTimeCost zero", b.meetingEndTimeCost, 0);
    }
    {
        MeetingSchedule s = makeBase(1, 12, {5});
        addMeeting(s, 4, {}, {}, 0, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("meetingEndTimeCost", b.meetingEndTimeCost, 3);
    }
    // soft 9: consecutiveMeetingsCost — overlap (start equal) is
    // not back-to-back (Java: 0); end 3 then start 4 is (Java: 1).
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 0, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("consecutiveMeetingsCost unpenalized (overlap)",
                   b.consecutiveMeetingsCost,
                   0);
    }
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 4, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount(
            "consecutiveMeetingsCost penalized", b.consecutiveMeetingsCost, 1);
    }
    // soft 10: meetingOverlapCost — [1,4) vs [0,3): overlap 2 (Java: 2).
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 3, {}, {}, 1, 0);
        addMeeting(s, 3, {}, {}, 0, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("meetingOverlapCost", b.meetingOverlapCost, 2);
    }
    // soft 11: largerRoomCapacityCost — capacity-1 room while a capacity-2
    // room exists: 1 (Java: 1); in the largest room: 0.
    {
        MeetingSchedule s = makeBase(1, 12, {1, 2});
        addMeeting(s, 4, {}, {}, 0, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount(
            "largerRoomCapacityCost penalized", b.largerRoomCapacityCost, 1);
    }
    {
        MeetingSchedule s = makeBase(1, 12, {1, 2});
        addMeeting(s, 4, {}, {}, 0, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount(
            "largerRoomCapacityCost largest room", b.largerRoomCapacityCost, 0);
    }
    // soft 12: roomChangeCost — shared attendee, different rooms, gap 0
    // grains (end 3, start 4): 1 (Java: 1); gap 4 grains: 0 (Java: 0).
    {
        MeetingSchedule s = makeBase(1, 16, {5, 5});
        addMeeting(s, 4, {0}, {}, 0, 0);
        addMeeting(s, 4, {0}, {}, 4, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("roomChangeCost penalized", b.roomChangeCost, 1);
    }
    {
        MeetingSchedule s = makeBase(1, 16, {5, 5});
        addMeeting(s, 4, {0}, {}, 0, 0);
        addMeeting(s, 4, {0}, {}, 8, 1);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("roomChangeCost unpenalized (gap > 2)", b.roomChangeCost, 0);
    }
    // soft 12: same room — stable, no penalty.
    {
        MeetingSchedule s = makeBase(1, 16, {5, 5});
        addMeeting(s, 4, {0}, {}, 0, 0);
        addMeeting(s, 4, {0}, {}, 4, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("roomChangeCost same room", b.roomChangeCost, 0);
    }
    // Score mapping: hard = -(hard sum), soft = -(medium * 1e6 + raw soft).
    {
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {0}, {0 + 1}, 0, 0);  // people 0 required, 1 pref
        addMeeting(s, 4, {0}, {1}, 2, 0);      // same room, overlap 2
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        const Score score = ScoreCalculator::score(s);
        checkCount("mapping: hard", score.hard, -b.hardTotal());
        const double expectedSoft
            = -(static_cast<double>(b.mediumTotal()) * kMediumScale
                + static_cast<double>(b.softTotal()));
        check(std::fabs(score.soft - expectedSoft) < 1e-9,
              "mapping: soft = -(medium * 1e6 + raw soft)");
        checkCount("mapping: medium present", b.mediumTotal() > 0 ? 1 : 0, 1);
    }
    // Unassigned assignments contribute nothing.
    {
        MeetingSchedule s = makeBase(1, 12, {1});
        addMeeting(s, 4, {0, 1, 2}, {}, -1);  // would violate capacity
        const Score score = ScoreCalculator::score(s);
        check(score.hard == 0 && score.soft == 0.0,
              "unassigned contributes nothing");
    }
    // Pinned assignments are scored like any other.
    {
        MeetingSchedule s = makeBase(1, 12, {5});
        addMeeting(s, 4, {}, {}, 0, 0);
        s.meetingAssignments.back().pinned = true;
        addMeeting(s, 4, {}, {}, 2, 0);
        const Breakdown b = ScoreCalculator::computeBreakdown(s);
        checkCount("pinned assignment still scored", b.roomConflictCost, 2);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random mid-size instance for the incremental cross-check and the solves.
// -------------------------------------------------------------------------
MeetingSchedule buildRandomInstance(int meetingCount, std::uint64_t seed)
{
    MeetingSchedule s
        = makeBase(/*days*/ 2, /*grainsPerDay*/ 14, {8, 5, 3}, /*people*/ 8);
    std::mt19937_64 rng(seed);
    for (int i = 0; i < meetingCount; ++i)
    {
        const int duration = 1 + static_cast<int>(rng() % 6);
        std::vector<int> required;
        std::vector<int> preferred;
        const int requiredCount = 1 + static_cast<int>(rng() % 3);
        for (int k = 0; k < requiredCount; ++k)
        {
            const int person = static_cast<int>(rng() % s.people.size());
            bool duplicate = false;
            for (int existing : required)
                duplicate = duplicate || existing == person;
            if (!duplicate)
                required.push_back(person);
        }
        const int preferredCount = static_cast<int>(rng() % 3);
        for (int k = 0; k < preferredCount; ++k)
        {
            const int person = static_cast<int>(rng() % s.people.size());
            bool duplicate = false;
            for (int existing : preferred)
                duplicate = duplicate || existing == person;
            for (int existing : required)
                duplicate = duplicate || existing == person;
            if (!duplicate)
                preferred.push_back(person);
        }
        addMeeting(s, duration, required, preferred);
    }
    return s;
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves (including unassignments), driven through the Editor so
// the before/set/after hook contract is exercised exactly like in the engine.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Verifying incremental scoring == full recompute ==\n");
    MeetingSchedule s = buildRandomInstance(/*meetingCount*/ 18, /*seed*/ 7);

    MeetingSchedulingIncrementalScoreCalculator calc;
    optimaru::Editor<MeetingSchedule> editor(s, calc);
    MeetingSchedule shadow = s;

    std::mt19937_64 rng(123);
    const int assignmentCount = static_cast<int>(s.meetingAssignments.size());
    const int value_count = s.grainRoomCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            // ChangeMove; 10% of the time to unassigned (-1), like the
            // construction heuristic's starting state.
            const int entity = static_cast<int>(rng() % assignmentCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % value_count);
            editor.assign(entity, value);
        }
        else
        {
            // SwapMove.
            const int a = static_cast<int>(rng() % assignmentCount);
            const int b = static_cast<int>(rng() % assignmentCount);
            const int va = s.meetingAssignments[a].grainRoom;
            const int vb = s.meetingAssignments[b].grainRoom;
            editor.assign(a, vb);
            editor.assign(b, va);
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < assignmentCount; ++i)
        {
            shadow.meetingAssignments[i].grainRoom
                = s.meetingAssignments[i].grainRoom;
        }
        const Score full = ScoreCalculator::score(shadow);
        const Score incremental = editor.score();
        if (full.hard != incremental.hard
            || std::fabs(full.soft - incremental.soft) > 1e-6)
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
// (c) ScoreChecks::Full solve: the engine cross-checks the incremental score
// against a full reset + recompute after every single move and throws
// ScoreMismatchError on any divergence.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (c) FULL_ASSERT solve ==\n");
    MeetingSchedule s = buildRandomInstance(/*meetingCount*/ 12, /*seed*/ 11);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(2);
    config.seed = 3;
    config.score_checks = optimaru::ScoreChecks::Full;
    // Kick early so the LNS ruin/recreate path also runs under ScoreChecks::Full.
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    bool corrupted = false;
    Score final{};
    try
    {
        Solver solver(config);
        solver.solve(s);
        final = s.score;
    }
    catch (const optimaru::ScoreMismatchError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::score(s);
        check(final.hard == recomputed.hard
                  && std::fabs(final.soft - recomputed.soft) < 1e-6,
              "solver-reported score == full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Pinned assignments keep their value through a solve; the Java demo
// reproduction reaches hard 0 within the test budget.
// -------------------------------------------------------------------------
void verifyPinnedAndDemoSolve()
{
    std::printf("== (d) Pinned assignment + demo solve ==\n");
    MeetingSchedule s = buildRandomInstance(/*meetingCount*/ 8, /*seed*/ 21);
    const int pinnedValue = s.grainRoomOf(/*grainPos*/ 6, /*room*/ 2);
    s.meetingAssignments[0].grainRoom = pinnedValue;
    s.meetingAssignments[0].pinned = true;

    Solver::Config config;
    config.time_limit = std::chrono::seconds(2);
    config.seed = 5;
    Solver solver(config);
    solver.solve(s);

    check(s.meetingAssignments[0].grainRoom == pinnedValue,
          "pinned assignment kept its grainRoom");
    bool allAssigned = true;
    for (const MeetingAssignment &assignment : s.meetingAssignments)
    {
        if (assignment.grainRoom < 0)
            allAssigned = false;
    }
    check(allAssigned, "every assignment assigned after the solve");
    check(s.score.hard == 0, "small instance reaches 0hard");

    // Demo data: Java DemoDataGenerator reproduction, structural checks +
    // a short solve to feasibility (stop at first feasible).
    MeetingSchedule demo = buildDemoData();
    check(demo.people.size() == 20 && demo.timeGrains.size() == 160
              && demo.rooms.size() == 3 && demo.meetings.size() == 24
              && demo.meetingAssignments.size() == 24,
          "demo data sizes match the Java generator");
    int with8 = 0;
    int with12 = 0;
    int with16 = 0;
    for (const Meeting &meeting : demo.meetings)
    {
        with8 += meeting.durationInGrains == 8;
        with12 += meeting.durationInGrains == 12;
        with16 += meeting.durationInGrains == 16;
    }
    check(with8 == 10 && with12 == 7 && with16 == 7,
          "demo durations: 10x8 + 7x12 + 7x16 grains");

    Solver::Config demoConfig;
    demoConfig.time_limit = std::chrono::seconds(10);
    demoConfig.seed = 1;
    demoConfig.target_score
        = Score{0, -std::numeric_limits<double>::infinity()};
    Solver demoSolver(demoConfig);
    demoSolver.solve(demo);
    check(demo.score.hard == 0, "demo reaches 0hard within 10s");

    // The day-fit value recommender is pure move guidance: switching it off
    // must not change what a solve is allowed to reach (only how fast).
    MeetingSchedule unguided = buildDemoData();
    Solver::Config unguidedConfig = demoConfig;
    unguidedConfig.dayFitValueProbability = 0.0;
    Solver unguidedSolver(unguidedConfig);
    unguidedSolver.solve(unguided);
    check(unguided.score.hard == 0,
          "demo reaches 0hard with day-fit guidance disabled");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifyPinnedAndDemoSolve();

    if (failures == 0)
    {
        std::printf("All meeting-scheduling checks PASSED.\n");
        return 0;
    }
    std::printf("%d meeting-scheduling check(s) FAILED.\n", failures);
    return 1;
}

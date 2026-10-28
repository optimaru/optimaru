// Plugin-assembly (ms/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 400 random
//       assignments (unassigned meetings included)
//   (b) incremental score == full recompute across 2000 random moves driven
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

#include "ms/constraints.hpp"
#include "ms/domain.hpp"
#include "ms/score_calculator.hpp"
#include "ms/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

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

void checkScore(const std::string &label,
                const Score &actual,
                long long expectedHard,
                double expectedSoft)
{
    const bool ok = (actual.hard == expectedHard
                     && std::fabs(actual.soft - expectedSoft) < 1e-6);
    std::printf("  [%s] %-48s got=%s expected=%lldhard/%gsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedSoft);
    if (!ok)
        ++failures;
}

bool sameScore(const Score &a, const Score &b)
{
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-6;
}

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

Score scoreWith(DecomposedCalculator &calculator, MeetingSchedule &s)
{
    ScoreCalculator::prepare(s);  // idempotent; the components require it
    calculator.resetWorkingSolution(s);
    return calculator.calculateScore();
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    MeetingSchedule s = buildRandomInstance(/*meetings*/ 14, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int mismatches = 0;
    const int rounds = 400;
    for (int round = 0; round < rounds; ++round)
    {
        for (MeetingAssignment &assignment : s.meetingAssignments)
        {
            assignment.grainRoom
                = unit(rng) < 0.10
                      ? -1
                      : static_cast<int>(rng() % s.grainRoomCount());
        }
        const Score plugin = scoreWith(calculator, s);
        const Score monolithic = ScoreCalculator::calculateScore(s);
        if (!sameScore(plugin, monolithic))
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
    MeetingSchedule s = buildRandomInstance(/*meetings*/ 14, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<MeetingSchedule> director(s, calculator);
    MeetingSchedule shadow = s;

    std::mt19937_64 rng(123);
    const int assignmentCount = static_cast<int>(s.meetingAssignments.size());
    const int valueCount = s.grainRoomCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 2000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            const int assignment = static_cast<int>(rng() % assignmentCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(assignment, value);
        }
        else
        {
            const int a = static_cast<int>(rng() % assignmentCount);
            const int b = static_cast<int>(rng() % assignmentCount);
            const int va = s.meetingAssignments[a].grainRoom;
            const int vb = s.meetingAssignments[b].grainRoom;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        for (int i = 0; i < assignmentCount; ++i)
            shadow.meetingAssignments[i].grainRoom
                = s.meetingAssignments[i].grainRoom;
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = director.calculateScore();
        if (!sameScore(full, incremental))
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
// (c) Single-component assemblies on the Java ConstraintVerifier shapes.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Single-component assemblies ==\n");

    {  // roomConflict alone: same room, overlapping by 2 grains.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 2, 0);
        DecomposedCalculator calculator;
        calculator.emplace<RoomConflict>();
        checkScore("roomConflict alone", scoreWith(calculator, s), -2, 0.0);
    }
    {  // Different rooms: silent.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<RoomConflict>();
        checkScore(
            "roomConflict across rooms", scoreWith(calculator, s), 0, 0.0);
    }
    {  // avoidOvertime alone: a 4-grain meeting starting at the last grain of
        // a 4-grain calendar ends at index 4, where no grain exists.
        MeetingSchedule s = makeBase(1, 4, {5});
        addMeeting(s, 4, {}, {}, 1, 0);
        DecomposedCalculator calculator;
        calculator.emplace<AvoidOvertime>();
        checkScore("avoidOvertime alone", scoreWith(calculator, s), -4, 0.0);
    }
    {  // requiredRoomCapacity alone: 3 required attendees in a room for 2.
        MeetingSchedule s = makeBase(1, 8, {2});
        addMeeting(s, 2, {0, 1, 2}, {}, 0, 0);
        DecomposedCalculator calculator;
        calculator.emplace<RequiredRoomCapacity>();
        checkScore(
            "requiredRoomCapacity alone", scoreWith(calculator, s), -1, 0.0);
    }
    {  // requiredAttendanceConflict alone: two overlapping meetings sharing a
        // required attendee.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {0}, {}, 0, 0);
        addMeeting(s, 4, {0}, {}, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<RequiredAttendanceConflict>();
        checkScore("requiredAttendanceConflict alone",
                   scoreWith(calculator, s),
                   -2,
                   0.0);
    }
    {  // preferredAttendanceConflict alone: the same shape on the medium
        // level, folded at kMediumScale.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {0}, 0, 0);
        addMeeting(s, 4, {}, {0}, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<PreferredAttendanceConflict>();
        checkScore("preferredAttendanceConflict alone",
                   scoreWith(calculator, s),
                   0,
                   -2.0 * kMediumScale);
    }
    {  // requiredAndPreferredAttendanceConflict alone: required in one,
        // preferred in the other.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {0}, {}, 0, 0);
        addMeeting(s, 4, {}, {0}, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<RequiredAndPreferredAttendanceConflict>();
        checkScore("requiredAndPreferredAttendanceConflict alone",
                   scoreWith(calculator, s),
                   0,
                   -2.0 * kMediumScale);
    }
    {  // oneBreakBetweenConsecutiveMeetings alone: back to back, no gap.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 4, 1);
        DecomposedCalculator calculator;
        calculator.emplace<OneBreakBetweenConsecutiveMeetings>();
        checkScore("oneBreakBetweenConsecutiveMeetings alone",
                   scoreWith(calculator, s),
                   0,
                   -100.0);
    }
    {  // overlappingMeetings alone: 2 shared grains at x 10.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<OverlappingMeetings>();
        checkScore(
            "overlappingMeetings alone", scoreWith(calculator, s), 0, -20.0);
    }
    {  // roomStability alone: a shared attendee moves rooms with a 1-grain
        // gap (<= 2), so somebody has to walk.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 2, {0}, {}, 0, 0);
        addMeeting(s, 2, {0}, {}, 3, 1);
        DecomposedCalculator calculator;
        calculator.emplace<RoomStability>();
        checkScore("roomStability alone", scoreWith(calculator, s), 0, -1.0);
    }
    {  // A wide gap: no walk, no penalty.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 2, {0}, {}, 0, 0);
        addMeeting(s, 2, {0}, {}, 8, 1);
        DecomposedCalculator calculator;
        calculator.emplace<RoomStability>();
        checkScore(
            "roomStability with a wide gap", scoreWith(calculator, s), 0, 0.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    MeetingSchedule s = makeBase(1, 12, {5, 5});
    addMeeting(s, 4, {}, {}, 0, 0);
    addMeeting(s, 4, {}, {}, 2, 0);

    {
        DecomposedCalculator calculator;
        calculator.emplace<RoomConflict>(10.0);
        checkScore("roomConflict re-weighted to 10",
                   scoreWith(calculator, s),
                   -20,
                   0.0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<RoomConflict>();
        component->isHard = false;
        calculator.add(std::move(component));
        checkScore(
            "roomConflict demoted to soft", scoreWith(calculator, s), 0, -2.0);
    }
    {
        // exponent 2 shapes the raw overlap (2 grains) before the weight.
        DecomposedCalculator calculator;
        auto component = std::make_unique<RoomConflict>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore(
            "roomConflict with exponent 2", scoreWith(calculator, s), -4, 0.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    MeetingSchedule s = buildRandomInstance(/*meetings*/ 10, /*seed*/ 13);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(5);
    for (MeetingAssignment &assignment : s.meetingAssignments)
        assignment.grainRoom = static_cast<int>(rng() % s.grainRoomCount());

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    const Score score = scoreWith(calculator, s);

    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-3,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == 12,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    MeetingSchedule s = buildRandomInstance(/*meetings*/ 8, /*seed*/ 11);

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
        check(sameScore(s.score, recomputed),
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
        std::printf("All meeting-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d meeting-scheduling plugin check(s) FAILED.\n", failures);
    return 1;
}

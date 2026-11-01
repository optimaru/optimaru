// Plugin-assembly (ms/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 400 random
//       assignments (unassigned meetings included)
//   (b) incremental score == full recompute across 2000 random moves driven
//       through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: each component in isolation, hand-computed
//   (d) weight / level / exponent overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(schedule, calculator) under FULL_ASSERT
//   (g) raw costs by hand through constraintCosts(): one positive and one
//       zero case per constraint
//   (h) the catalog (defaultConstraints().info()) == the components'
//       kDefaultLevel / kDefaultWeight
//   (i) the medium level: the default fold, hard() / soft() / weight on a
//       medium constraint, and incremental == full with those overrides
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <utility>
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

    {  // roomConflictCost alone: same room, overlapping by 2 grains.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 2, 0);
        DecomposedCalculator calculator;
        calculator.emplace<RoomConflictCost>();
        checkScore("roomConflictCost alone", scoreWith(calculator, s), -2, 0.0);
    }
    {  // Different rooms: silent.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<RoomConflictCost>();
        checkScore(
            "roomConflictCost across rooms", scoreWith(calculator, s), 0, 0.0);
    }
    {  // overtimeCost alone: a 4-grain meeting starting at the last grain of
        // a 4-grain calendar ends at index 4, where no grain exists.
        MeetingSchedule s = makeBase(1, 4, {5});
        addMeeting(s, 4, {}, {}, 1, 0);
        DecomposedCalculator calculator;
        calculator.emplace<OvertimeCost>();
        checkScore("overtimeCost alone", scoreWith(calculator, s), -4, 0.0);
    }
    {  // roomCapacityCost alone: 3 required attendees in a room for 2.
        MeetingSchedule s = makeBase(1, 8, {2});
        addMeeting(s, 2, {0, 1, 2}, {}, 0, 0);
        DecomposedCalculator calculator;
        calculator.emplace<RoomCapacityCost>();
        checkScore("roomCapacityCost alone", scoreWith(calculator, s), -1, 0.0);
    }
    {  // requiredAttendanceConflictCost alone: two overlapping meetings
        // sharing a required attendee.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {0}, {}, 0, 0);
        addMeeting(s, 4, {0}, {}, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<RequiredAttendanceConflictCost>();
        checkScore("requiredAttendanceConflictCost alone",
                   scoreWith(calculator, s),
                   -2,
                   0.0);
    }
    {  // preferredAttendanceConflictCost alone: the same shape on the medium
        // level, folded at kMediumScale.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {0}, 0, 0);
        addMeeting(s, 4, {}, {0}, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<PreferredAttendanceConflictCost>();
        checkScore("preferredAttendanceConflictCost alone",
                   scoreWith(calculator, s),
                   0,
                   -2.0 * kMediumScale);
    }
    {  // requiredAndPreferredAttendanceConflictCost alone: required in one,
        // preferred in the other.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {0}, {}, 0, 0);
        addMeeting(s, 4, {}, {0}, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<RequiredAndPreferredAttendanceConflictCost>();
        checkScore("requiredAndPreferredAttendanceConflictCost alone",
                   scoreWith(calculator, s),
                   0,
                   -2.0 * kMediumScale);
    }
    {  // consecutiveMeetingsCost alone: back to back, no gap.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 4, 1);
        DecomposedCalculator calculator;
        calculator.emplace<ConsecutiveMeetingsCost>();
        checkScore("consecutiveMeetingsCost alone",
                   scoreWith(calculator, s),
                   0,
                   -100.0);
    }
    {  // meetingOverlapCost alone: 2 shared grains at x 10.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 4, {}, {}, 0, 0);
        addMeeting(s, 4, {}, {}, 2, 1);
        DecomposedCalculator calculator;
        calculator.emplace<MeetingOverlapCost>();
        checkScore(
            "meetingOverlapCost alone", scoreWith(calculator, s), 0, -20.0);
    }
    {  // roomChangeCost alone: a shared attendee moves rooms with a 1-grain
        // gap (<= 2), so somebody has to walk.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 2, {0}, {}, 0, 0);
        addMeeting(s, 2, {0}, {}, 3, 1);
        DecomposedCalculator calculator;
        calculator.emplace<RoomChangeCost>();
        checkScore("roomChangeCost alone", scoreWith(calculator, s), 0, -1.0);
    }
    {  // A wide gap: no walk, no penalty.
        MeetingSchedule s = makeBase(1, 12, {5, 5});
        addMeeting(s, 2, {0}, {}, 0, 0);
        addMeeting(s, 2, {0}, {}, 8, 1);
        DecomposedCalculator calculator;
        calculator.emplace<RoomChangeCost>();
        checkScore(
            "roomChangeCost with a wide gap", scoreWith(calculator, s), 0, 0.0);
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
        calculator.emplace<RoomConflictCost>(10.0);
        checkScore("roomConflictCost re-weighted to 10",
                   scoreWith(calculator, s),
                   -20,
                   0.0);
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<RoomConflictCost>();
        component->level = optimaru::Level::Soft;
        calculator.add(std::move(component));
        checkScore("roomConflictCost demoted to soft",
                   scoreWith(calculator, s),
                   0,
                   -2.0);
    }
    {
        // exponent 2 shapes the raw overlap (2 grains) before the weight.
        DecomposedCalculator calculator;
        auto component = std::make_unique<RoomConflictCost>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("roomConflictCost with exponent 2",
                   scoreWith(calculator, s),
                   -4,
                   0.0);
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

    // A medium total is in medium units (not multiplied by the fold), so it
    // is folded here the way the calculator folds its medium accumulator.
    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
    {
        if (total.level == optimaru::Level::Hard)
            hard += total.cost;
        else if (total.level == optimaru::Level::Medium)
            soft += total.cost * kMediumScale;
        else
            soft += total.cost;
    }
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

// -------------------------------------------------------------------------
// (g) Raw costs by hand: one positive and one zero case per constraint,
// read through constraintCosts() (unweighted, before exponent and level).
// -------------------------------------------------------------------------
double rawTotal(MeetingSchedule &s, const std::string &name)
{
    ScoreCalculator::prepare(s);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(s);
    calculator.calculateScore();
    for (const auto &entry : calculator.constraintCosts())
    {
        if (entry.name != name)
            continue;
        double total = 0.0;
        for (double cost : entry.unitCosts)
            total += cost;
        return total;
    }
    return -1.0;  // unknown name: never a raw cost of this domain
}

void checkRaw(const std::string &label,
              MeetingSchedule s,
              const std::string &name,
              double expected)
{
    const double actual = rawTotal(s, name);
    const bool ok = actual == expected;
    std::printf("  [%s] %-56s got=%g expected=%g\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual,
                expected);
    if (!ok)
        ++failures;
}

void verifyRawCostsByHand()
{
    std::printf("== (g) Raw costs by hand (positive / zero) ==\n");
    {  // Same room, [0,4) and [2,6): 2 shared grains; back to back: 0.
        MeetingSchedule positive = makeBase(1, 12, {5, 5});
        addMeeting(positive, 4, {}, {}, 0, 0);
        addMeeting(positive, 4, {}, {}, 2, 0);
        checkRaw("roomConflictCost: 2 shared grains",
                 positive,
                 "roomConflictCost",
                 2.0);
        MeetingSchedule zero = makeBase(1, 12, {5, 5});
        addMeeting(zero, 4, {}, {}, 0, 0);
        addMeeting(zero, 4, {}, {}, 4, 0);
        checkRaw(
            "roomConflictCost: back to back", zero, "roomConflictCost", 0.0);
    }
    {  // Grains 0..3; a 4-grain meeting from grain 2 ends at index 5, which
        // does not exist: the raw cost is that last index, 5. From grain 0
        // it ends on grain 3: 0.
        MeetingSchedule positive = makeBase(1, 4, {5});
        addMeeting(positive, 4, {}, {}, 2, 0);
        checkRaw("overtimeCost: last index 5", positive, "overtimeCost", 5.0);
        MeetingSchedule zero = makeBase(1, 4, {5});
        addMeeting(zero, 4, {}, {}, 0, 0);
        checkRaw("overtimeCost: ends on grain 3", zero, "overtimeCost", 0.0);
    }
    {  // Person 0 required at [0,4) and [2,6): 2 person-grains.
        MeetingSchedule positive = makeBase(1, 12, {5, 5});
        addMeeting(positive, 4, {0}, {}, 0, 0);
        addMeeting(positive, 4, {0}, {}, 2, 1);
        checkRaw("requiredAttendanceConflictCost: 2 person-grains",
                 positive,
                 "requiredAttendanceConflictCost",
                 2.0);
        MeetingSchedule zero = makeBase(1, 12, {5, 5});
        addMeeting(zero, 4, {0}, {}, 0, 0);
        addMeeting(zero, 4, {0}, {}, 4, 1);
        checkRaw("requiredAttendanceConflictCost: back to back",
                 zero,
                 "requiredAttendanceConflictCost",
                 0.0);
    }
    {  // 1 required + 1 preferred = 2 seats: capacity 1 is 1 short, 2 fits.
        MeetingSchedule positive = makeBase(1, 12, {1});
        addMeeting(positive, 4, {0}, {1}, 0, 0);
        checkRaw("roomCapacityCost: 1 seat short",
                 positive,
                 "roomCapacityCost",
                 1.0);
        MeetingSchedule zero = makeBase(1, 12, {2});
        addMeeting(zero, 4, {0}, {1}, 0, 0);
        checkRaw("roomCapacityCost: fits", zero, "roomCapacityCost", 0.0);
    }
    {  // 2 days x 4 grains: from grain 2 (day 100) to grain 5 (day 101).
        MeetingSchedule positive = makeBase(2, 4, {5});
        addMeeting(positive, 4, {}, {}, 2, 0);
        checkRaw("dayCrossingCost: crosses into day 101",
                 positive,
                 "dayCrossingCost",
                 1.0);
        MeetingSchedule zero = makeBase(2, 4, {5});
        addMeeting(zero, 4, {}, {}, 0, 0);
        checkRaw(
            "dayCrossingCost: within day 100", zero, "dayCrossingCost", 0.0);
    }
    {  // Person 0 required at one, preferred at the other, full overlap of 4.
        MeetingSchedule positive = makeBase(1, 12, {5, 5});
        addMeeting(positive, 4, {0}, {}, 0, 0);
        addMeeting(positive, 4, {}, {0}, 0, 1);
        checkRaw("requiredAndPreferredAttendanceConflictCost: 4",
                 positive,
                 "requiredAndPreferredAttendanceConflictCost",
                 4.0);
        MeetingSchedule zero = makeBase(1, 12, {5, 5});
        addMeeting(zero, 4, {0}, {}, 0, 0);
        addMeeting(zero, 4, {}, {0}, 4, 1);
        checkRaw("requiredAndPreferredAttendanceConflictCost: back to back",
                 zero,
                 "requiredAndPreferredAttendanceConflictCost",
                 0.0);
    }
    {  // Person 0 preferred at both, full overlap of 4.
        MeetingSchedule positive = makeBase(1, 12, {5, 5});
        addMeeting(positive, 4, {}, {0}, 0, 0);
        addMeeting(positive, 4, {}, {0}, 0, 1);
        checkRaw("preferredAttendanceConflictCost: 4 person-grains",
                 positive,
                 "preferredAttendanceConflictCost",
                 4.0);
        MeetingSchedule zero = makeBase(1, 12, {5, 5});
        addMeeting(zero, 4, {}, {0}, 0, 0);
        addMeeting(zero, 4, {}, {0}, 4, 1);
        checkRaw("preferredAttendanceConflictCost: back to back",
                 zero,
                 "preferredAttendanceConflictCost",
                 0.0);
    }
    {  // [0,4) ends on grain 3; a 1-grain meeting at grain 0 ends on 0.
        MeetingSchedule positive = makeBase(1, 12, {5});
        addMeeting(positive, 4, {}, {}, 0, 0);
        checkRaw("meetingEndTimeCost: last index 3",
                 positive,
                 "meetingEndTimeCost",
                 3.0);
        MeetingSchedule zero = makeBase(1, 12, {5});
        addMeeting(zero, 1, {}, {}, 0, 0);
        checkRaw("meetingEndTimeCost: ends on grain 0",
                 zero,
                 "meetingEndTimeCost",
                 0.0);
    }
    {  // [0,4) then [4,8): one back-to-back pair; both from grain 0: none.
        MeetingSchedule positive = makeBase(1, 12, {5, 5});
        addMeeting(positive, 4, {}, {}, 0, 0);
        addMeeting(positive, 4, {}, {}, 4, 1);
        checkRaw("consecutiveMeetingsCost: one pair",
                 positive,
                 "consecutiveMeetingsCost",
                 1.0);
        MeetingSchedule zero = makeBase(1, 12, {5, 5});
        addMeeting(zero, 4, {}, {}, 0, 0);
        addMeeting(zero, 4, {}, {}, 0, 1);
        checkRaw("consecutiveMeetingsCost: overlapping, not adjacent",
                 zero,
                 "consecutiveMeetingsCost",
                 0.0);
    }
    {  // [1,4) vs [0,3): 2 shared grains in different rooms; [0,4)/[4,8): 0.
        MeetingSchedule positive = makeBase(1, 12, {5, 5});
        addMeeting(positive, 3, {}, {}, 1, 0);
        addMeeting(positive, 3, {}, {}, 0, 1);
        checkRaw("meetingOverlapCost: 2 shared grains",
                 positive,
                 "meetingOverlapCost",
                 2.0);
        MeetingSchedule zero = makeBase(1, 12, {5, 5});
        addMeeting(zero, 4, {}, {}, 0, 0);
        addMeeting(zero, 4, {}, {}, 4, 1);
        checkRaw("meetingOverlapCost: back to back",
                 zero,
                 "meetingOverlapCost",
                 0.0);
    }
    {  // Rooms of capacity 1 and 2: the small one leaves 2 - 1 = 1 unused in
        // the larger room; the largest room leaves nothing.
        MeetingSchedule positive = makeBase(1, 12, {1, 2});
        addMeeting(positive, 4, {}, {}, 0, 0);
        checkRaw("largerRoomCapacityCost: 1 seat in a larger room",
                 positive,
                 "largerRoomCapacityCost",
                 1.0);
        MeetingSchedule zero = makeBase(1, 12, {1, 2});
        addMeeting(zero, 4, {}, {}, 0, 1);
        checkRaw("largerRoomCapacityCost: largest room",
                 zero,
                 "largerRoomCapacityCost",
                 0.0);
    }
    {  // Person 0 goes from room 0 [0,4) to room 1 [4,8): gap 0 <= 2, one
        // switch; with the second meeting at [8,12) the gap is 4: none.
        MeetingSchedule positive = makeBase(1, 16, {5, 5});
        addMeeting(positive, 4, {0}, {}, 0, 0);
        addMeeting(positive, 4, {0}, {}, 4, 1);
        checkRaw("roomChangeCost: one attendee switch",
                 positive,
                 "roomChangeCost",
                 1.0);
        MeetingSchedule zero = makeBase(1, 16, {5, 5});
        addMeeting(zero, 4, {0}, {}, 0, 0);
        addMeeting(zero, 4, {0}, {}, 8, 1);
        checkRaw(
            "roomChangeCost: gap of 4 grains", zero, "roomChangeCost", 0.0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) The catalog read off the default assembly == the components' constants.
// -------------------------------------------------------------------------
template <typename Component>
bool rowIs(const optimaru::ConstraintInfo &row,
           const char *name,
           optimaru::Level level,
           double weight,
           bool isGlobal)
{
    return row.name == name && row.defaultLevel == Component::kDefaultLevel
           && row.defaultWeight == Component::kDefaultWeight
           && row.defaultLevel == level && row.defaultWeight == weight
           && row.levelFlippable && !row.reward && row.isGlobal == isGlobal
           && row.defaultEnabled;
}

void verifyCatalog()
{
    std::printf("== (h) Catalog == component constants ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 12, "12 catalog rows");
    if (rows.size() != 12)
        return;
    // Unit constraints first, then the globals, in emplace order: the
    // list_constraints() order.
    check(rowIs<OvertimeCost>(rows[0], "overtimeCost", Level::Hard, 1.0, false)
              && rowIs<RoomCapacityCost>(
                  rows[1], "roomCapacityCost", Level::Hard, 1.0, false)
              && rowIs<DayCrossingCost>(
                  rows[2], "dayCrossingCost", Level::Hard, 1.0, false)
              && rowIs<MeetingEndTimeCost>(
                  rows[3], "meetingEndTimeCost", Level::Soft, 1.0, false)
              && rowIs<LargerRoomCapacityCost>(
                  rows[4], "largerRoomCapacityCost", Level::Soft, 1.0, false)
              && rowIs<RoomConflictCost>(
                  rows[5], "roomConflictCost", Level::Hard, 1.0, true)
              && rowIs<RequiredAttendanceConflictCost>(
                  rows[6],
                  "requiredAttendanceConflictCost",
                  Level::Hard,
                  1.0,
                  true)
              && rowIs<RequiredAndPreferredAttendanceConflictCost>(
                  rows[7],
                  "requiredAndPreferredAttendanceConflictCost",
                  Level::Medium,
                  1.0,
                  true)
              && rowIs<PreferredAttendanceConflictCost>(
                  rows[8],
                  "preferredAttendanceConflictCost",
                  Level::Medium,
                  1.0,
                  true)
              && rowIs<ConsecutiveMeetingsCost>(
                  rows[9], "consecutiveMeetingsCost", Level::Soft, 100.0, true)
              && rowIs<MeetingOverlapCost>(
                  rows[10], "meetingOverlapCost", Level::Soft, 10.0, true)
              && rowIs<RoomChangeCost>(
                  rows[11], "roomChangeCost", Level::Soft, 1.0, true),
          "info() == the components' kDefaultWeight / kDefaultLevel");
    check(kMediumFold.kind == optimaru::MediumFold::Kind::IntoSoft
              && kMediumFold.unit == kMediumScale && kMediumScale == 1e6,
          "the domain folds medium into soft at 1e6");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The medium level: requiredAndPreferredAttendanceConflictCost and
// preferredAttendanceConflictCost.
// -------------------------------------------------------------------------
DecomposedCalculator::LevelScores levelsWith(ConstraintAssembly assembly,
                                             MeetingSchedule &s)
{
    ScoreCalculator::prepare(s);
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    calculator.resetWorkingSolution(s);
    calculator.calculateScore();
    return calculator.levelScores();
}

void verifyMediumLevel()
{
    std::printf("== (i) Medium level ==\n");
    // Person 0 required at [0,4) in room 0 and preferred at [2,6) in room 1:
    // 2 required-and-preferred person-grains; person 1 preferred at both:
    // 2 preferred person-grains. Soft: last indices 3 + 5 = 8, larger rooms
    // 0, overlap 2 grains x 10 = 20, no back-to-back pair, and both people
    // switch rooms (the second meeting starts 2 grains before the first
    // ends, a gap <= 2): 2 -> 30.
    MeetingSchedule s = makeBase(1, 12, {5, 5}, /*people*/ 2);
    addMeeting(s, 4, {0}, {1}, 0, 0);
    addMeeting(s, 4, {}, {0, 1}, 2, 1);
    const Breakdown breakdown = ScoreCalculator::computeBreakdown(s);
    check(breakdown.requiredAndPreferredAttendanceConflictCost == 2
              && breakdown.preferredAttendanceConflictCost == 2
              && breakdown.hardTotal() == 0 && breakdown.softTotal() == 30,
          "the hand instance: 2 + 2 medium person-grains, 30 soft");
    const Score monolithic = ScoreCalculator::calculateScore(s);
    check(monolithic.hard == 0 && monolithic.soft == -30.0 - 4.0 * 1e6,
          "monolithic: medium folded into soft x 1e6");
    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        const Score engine = scoreWith(calculator, s);
        check(sameScore(engine, monolithic),
              "default assembly: the fold equals the monolithic score");
        const auto levels = calculator.levelScores();
        check(levels.hard == 0 && levels.medium == -4.0 && levels.soft == -30.0,
              "levelScores() reports the unfolded levels");
    }
    const char *mixed = "requiredAndPreferredAttendanceConflictCost";
    const char *preferred = "preferredAttendanceConflictCost";
    {
        const auto levels = levelsWith(defaultConstraints().hard(mixed), s);
        check(levels.hard == -2 && levels.medium == -2.0
                  && levels.soft == -30.0,
              "hard(): one hard unit per raw person-grain");
    }
    {
        const auto levels
            = levelsWith(defaultConstraints().soft(mixed).soft(preferred), s);
        check(levels.hard == 0 && levels.medium == 0.0 && levels.soft == -34.0,
              "soft(): one soft unit per raw person-grain, outside the fold");
    }
    {
        const auto levels
            = levelsWith(defaultConstraints().weight(preferred, 3), s);
        check(levels.hard == 0 && levels.medium == -8.0 && levels.soft == -30.0,
              "weight 3 stays in medium units");
    }
    std::printf("\n");
}

// Incremental == full recompute with the medium terms overridden, through
// the ScoreDirector protocol.
void verifyMediumIncremental()
{
    std::printf("== (i) Medium overrides: incremental == full ==\n");
    const char *mixed = "requiredAndPreferredAttendanceConflictCost";
    const char *preferred = "preferredAttendanceConflictCost";
    const std::vector<
        std::pair<std::string, std::function<ConstraintAssembly()>>>
        variants{
            {"default", [] { return defaultConstraints(); }},
            {"hard",
             [=] { return defaultConstraints().hard(mixed).hard(preferred); }},
            {"soft",
             [=] { return defaultConstraints().soft(mixed).soft(preferred); }},
            {"weight 3", [=] { return defaultConstraints().weight(mixed, 3); }},
        };
    for (const auto &variant : variants)
    {
        MeetingSchedule s = buildRandomInstance(/*meetings*/ 12, /*seed*/ 17);
        ScoreCalculator::prepare(s);
        DecomposedCalculator calculator;
        variant.second().into(calculator);
        optimaru::ScoreDirector<MeetingSchedule> director(s, calculator);
        std::mt19937_64 rng(41);
        const int assignmentCount
            = static_cast<int>(s.meetingAssignments.size());
        int mismatches = 0;
        int mediumSteps = 0;
        for (int step = 0; step < 600; ++step)
        {
            const int assignment = static_cast<int>(rng() % assignmentCount);
            const int value = static_cast<int>(rng() % s.grainRoomCount());
            director.changeVariable(assignment, value);
            const Score incremental = director.calculateScore();
            DecomposedCalculator fresh;
            variant.second().into(fresh);
            const Score full = scoreWith(fresh, s);
            if (fresh.levelScores().medium != 0.0)
                ++mediumSteps;
            if (!sameScore(incremental, full))
                ++mismatches;
        }
        check(mismatches == 0,
              "medium " + variant.first
                  + ": 600 random moves, incremental == full recompute");
        if (variant.first == "default" || variant.first == "weight 3")
            check(mediumSteps > 0,
                  variant.first + ": the walk visits medium states");
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

    if (failures == 0)
    {
        std::printf("All meeting-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d meeting-scheduling plugin check(s) FAILED.\n", failures);
    return 1;
}

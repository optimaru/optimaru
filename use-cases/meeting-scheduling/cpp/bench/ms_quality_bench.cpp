// Quality-benchmark driver for the C++ meeting-scheduling port.
//
// Solves either (a) the demo instance (ms/demo_data.hpp, unchanged) or (b) a
// deterministic scaled instance in the demo's shape (same duration mix, same
// attendee-count distribution, meetings/people/rooms scaled together so the
// occupancy density stays comparable) for a fixed wall-clock budget, then
// prints the best-score trajectory, the per-constraint breakdown and a
// trajectory-based moves/s estimate (the st_quality_bench convention).
//
// Usage: ms_quality_bench <demo|scaled:<meetings>> <seed> <seconds> [k=v ...]
// Knobs (bench-only overrides of Solver::Config, for A/B sweeps):
//   la= change= swap= alnsTrigger= alnsPolish= minRuin= maxRuin=
//   maxRuinCount= decay= dayFit= asMove= asMoveRuin=

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "ms/demo_data.hpp"
#include "ms/domain.hpp"
#include "ms/score_calculator.hpp"
#include "ms/solver.hpp"

using namespace ms;

namespace
{

// ---------------------------------------------------------------------------
// Scaled instance: the demo's shape at 2x / 4x. meetings, people and rooms all
// scale by the same factor over the demo's 24 / 20 / 3, on the same 4-day x 40
// grain calendar, so grain-room occupancy density and per-person conflict
// density stay in the demo's range. Duration mix (8/12/16 grains in thirds)
// and the required/preferred attendee-count share tables are the demo's.
// The generator seed is fixed, so every solver seed sees the same instance.
// ---------------------------------------------------------------------------
MeetingSchedule buildScaledSchedule(int meetingCount)
{
    std::mt19937_64 rng(9001);
    MeetingSchedule schedule;

    const double factor = static_cast<double>(meetingCount) / 24.0;
    const int personCount = std::max(4, static_cast<int>(20.0 * factor + 0.5));
    const int roomCount = std::max(2, static_cast<int>(3.0 * factor + 0.5));

    for (int i = 0; i < personCount; ++i)
    {
        schedule.people.push_back(
            {std::to_string(i), "Person " + std::to_string(i)});
    }

    constexpr int kFirstDayOfYear = 200;
    int count = 0;
    for (int day = 0; day < 4; ++day)
    {
        for (int minute = 8 * 60; minute <= 17 * 60 + 45; minute += 15)
        {
            ++count;
            schedule.timeGrains.push_back(
                {std::to_string(count), count, kFirstDayOfYear + day, minute});
        }
    }

    // Room capacities cycle the demo's 30 / 20 / 16 seats.
    const int capacities[] = {30, 20, 16};
    for (int r = 0; r < roomCount; ++r)
    {
        schedule.rooms.push_back({"R" + std::to_string(r + 1),
                                  "Room " + std::to_string(r + 1),
                                  capacities[r % 3]});
    }

    // Durations: thirds of 8 / 12 / 16 grains (leftovers 8), like the demo.
    std::vector<int> durations(meetingCount, 8);
    const int third = static_cast<int>(0.33 * meetingCount);
    for (int i = 0; i < third; ++i)
        durations[third + i] = 12;
    for (int i = 0; i < third; ++i)
        durations[2 * third + i] = 16;
    std::shuffle(durations.begin(), durations.end(), rng);

    // Attendee counts: the demo's share tables, clamped to the person count.
    const std::pair<double, int> requiredShares[] = {{0.36, 2},
                                                     {0.08, 3},
                                                     {0.02, 4},
                                                     {0.08, 5},
                                                     {0.10, 6},
                                                     {0.05, 7},
                                                     {0.05, 8},
                                                     {0.05, 10}};
    const std::pair<double, int> preferredShares[] = {{0.06, 1},
                                                      {0.20, 2},
                                                      {0.18, 3},
                                                      {0.06, 4},
                                                      {0.04, 5},
                                                      {0.04, 6},
                                                      {0.04, 7},
                                                      {0.04, 8},
                                                      {0.08, 9},
                                                      {0.04, 10}};
    std::vector<int> requiredCounts;
    std::vector<int> preferredCounts;
    for (const auto &[share, size] : requiredShares)
    {
        const int n = static_cast<int>(share * meetingCount);
        for (int i = 0; i < n; ++i)
            requiredCounts.push_back(size);
    }
    for (const auto &[share, size] : preferredShares)
    {
        const int n = static_cast<int>(share * meetingCount);
        for (int i = 0; i < n; ++i)
            preferredCounts.push_back(size);
    }
    requiredCounts.resize(meetingCount, 2);
    preferredCounts.resize(meetingCount, 0);
    std::shuffle(requiredCounts.begin(), requiredCounts.end(), rng);
    std::shuffle(preferredCounts.begin(), preferredCounts.end(), rng);

    for (int i = 0; i < meetingCount; ++i)
    {
        Meeting meeting;
        meeting.id = std::to_string(i);
        meeting.topic = "Meeting " + std::to_string(i);
        meeting.durationInGrains = durations[i];
        std::vector<int> taken;
        const int required = std::min(requiredCounts[i], personCount - 1);
        const int preferred
            = std::min(preferredCounts[i], personCount - 1 - required);
        while (static_cast<int>(meeting.requiredAttendees.size()) < required)
        {
            const int person = static_cast<int>(rng() % personCount);
            if (std::find(taken.begin(), taken.end(), person) == taken.end())
            {
                taken.push_back(person);
                meeting.requiredAttendees.push_back(person);
            }
        }
        while (static_cast<int>(meeting.preferredAttendees.size()) < preferred)
        {
            const int person = static_cast<int>(rng() % personCount);
            if (std::find(taken.begin(), taken.end(), person) == taken.end())
            {
                taken.push_back(person);
                meeting.preferredAttendees.push_back(person);
            }
        }
        schedule.meetings.push_back(std::move(meeting));

        MeetingAssignment assignment;
        assignment.id = std::to_string(i);
        assignment.meeting = i;
        schedule.meetingAssignments.push_back(std::move(assignment));
    }
    return schedule;
}

void printBreakdown(const Breakdown &b)
{
    struct Row
    {
        const char *name;
        std::int64_t units;
    };
    const Row rows[] = {
        {"H.roomConflict", b.roomConflictGrains},
        {"H.avoidOvertime", b.overtimeSum},
        {"H.requiredAttendanceConflict", b.requiredConflictGrains},
        {"H.requiredRoomCapacity", b.roomCapacityShortage},
        {"H.startAndEndOnSameDay", b.startEndDayViolations},
        {"M.requiredAndPreferredAttendanceConflict",
         b.requiredPreferredConflictGrains},
        {"M.preferredAttendanceConflict", b.preferredConflictGrains},
        {"S.doMeetingsAsSoonAsPossible", b.lastGrainIndexSum},
        {"S.oneBreakBetweenConsecutiveMeetings", b.consecutiveMeetingPairs},
        {"S.overlappingMeetings", b.overlappingMeetingGrains},
        {"S.assignLargerRoomsFirst", b.largerRoomCapacitySum},
        {"S.roomStability", b.roomStabilityViolations},
    };
    for (const Row &row : rows)
    {
        if (row.units != 0)
            std::printf("MS_BENCH breakdown %s units=%lld\n",
                        row.name,
                        static_cast<long long>(row.units));
    }
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr,
                     "usage: ms_quality_bench <demo|scaled:<meetings>> <seed> "
                     "<seconds> [key=value ...]\n");
        return 2;
    }
    const std::string instanceArg = argv[1];
    const std::uint64_t seed = std::strtoull(argv[2], nullptr, 10);
    const int seconds = std::atoi(argv[3]);

    MeetingSchedule schedule;
    std::string instanceName = instanceArg;
    if (instanceArg == "demo")
    {
        schedule = buildDemoData();
    }
    else if (instanceArg.rfind("scaled:", 0) == 0)
    {
        schedule = buildScaledSchedule(std::atoi(instanceArg.c_str() + 7));
    }
    else
    {
        std::fprintf(stderr, "unknown instance %s\n", instanceArg.c_str());
        return 2;
    }

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(seconds);
    config.randomSeed = seed;
    config.parallelSolverCount = 1;
    for (int a = 4; a < argc; ++a)
    {
        const char *eq = std::strchr(argv[a], '=');
        if (!eq)
        {
            std::fprintf(stderr, "bad knob %s\n", argv[a]);
            return 2;
        }
        const std::string key(argv[a], eq - argv[a]);
        const double value = std::atof(eq + 1);
        if (key == "la")
            config.lateAcceptanceSize = static_cast<std::size_t>(value);
        else if (key == "change")
            config.changeMoveWeight = value;
        else if (key == "swap")
            config.swapMoveWeight = value;
        else if (key == "alnsTrigger")
            config.ruinRecreateUnimprovedStepTrigger
                = static_cast<std::int64_t>(value);
        else if (key == "alnsPolish")
            config.ruinRecreatePolishSteps = static_cast<std::int64_t>(value);
        else if (key == "minRuin")
            config.ruinRecreateMinRuinFraction = value;
        else if (key == "maxRuin")
            config.ruinRecreateMaxRuinFraction = value;
        else if (key == "maxRuinCount")
            config.ruinRecreateMaxRuinCount = static_cast<int>(value);
        else if (key == "decay")
            config.ruinRecreateOperatorDecay = value;
        else if (key == "dayFit")
            config.dayFitValueProbability = value;
        else if (key == "asMove")
            config.ruinRecreateAsMoveWeight = value;
        else if (key == "asMoveRuin")
            config.ruinRecreateAsMoveMaxRuin = static_cast<int>(value);
        else
        {
            std::fprintf(stderr, "unknown knob %s\n", key.c_str());
            return 2;
        }
    }

    std::printf("MS_BENCH instance name=%s meetings=%zu people=%zu grains=%zu "
                "rooms=%zu seed=%llu budget=%ds\n",
                instanceName.c_str(),
                schedule.meetings.size(),
                schedule.people.size(),
                schedule.timeGrains.size(),
                schedule.rooms.size(),
                static_cast<unsigned long long>(seed),
                seconds);

    const auto t0 = std::chrono::steady_clock::now();
    long long lastStep = 0;
    long long lastMs = 0;
    long long feasibleMs = -1;
    config.onNewBest = [&t0, &lastStep, &lastMs, &feasibleMs](std::int64_t step,
                                                              const Score &sc)
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        lastStep = step;
        lastMs = ms;
        if (feasibleMs < 0 && sc.hard >= 0)
            feasibleMs = ms;
    };

    Solver solver(config);
    solver.solve(schedule);

    const double movesPerSec = lastMs > 0
                                   ? 1000.0 * static_cast<double>(lastStep)
                                         / static_cast<double>(lastMs)
                                   : 0.0;
    Breakdown breakdown = ScoreCalculator::computeBreakdown(schedule);
    printBreakdown(breakdown);
    std::printf("MS_BENCH_FINAL hard=%lld medium=%lld soft=%lld feasible=%s "
                "first_feasible_ms=%lld moves_per_sec=%.0f\n",
                static_cast<long long>(breakdown.hardTotal()),
                static_cast<long long>(breakdown.mediumTotal()),
                static_cast<long long>(breakdown.softTotal()),
                schedule.score.isFeasible() ? "true" : "false",
                feasibleMs,
                movesPerSec);
    return 0;
}

// Meeting-scheduling demo: reproduces the Java quickstart's demo data (see
// ms/demo_data.hpp), solves for N seconds (default 10) and prints the
// resulting schedule. Exits non-zero unless the solution is feasible
// (hard == 0).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ms/demo_data.hpp"
#include "ms/domain.hpp"
#include "ms/score_calculator.hpp"
#include "ms/solver.hpp"

using namespace ms;

namespace
{

std::string minuteToString(int minuteOfDay)
{
    char buf[8];
    std::snprintf(
        buf, sizeof(buf), "%02d:%02d", minuteOfDay / 60, minuteOfDay % 60);
    return buf;
}

void printSchedule(const MeetingSchedule &schedule)
{
    // Sort assignments by (grain position, room) for a readable listing.
    std::vector<int> order;
    for (int i = 0; i < static_cast<int>(schedule.meetingAssignments.size());
         ++i)
    {
        order.push_back(i);
    }
    std::sort(order.begin(),
              order.end(),
              [&](int a, int b)
              {
                  return schedule.meetingAssignments[a].grainRoom
                         < schedule.meetingAssignments[b].grainRoom;
              });

    std::printf("  %-4s %-6s %-6s %-8s %-6s %-30s %s\n",
                "day",
                "start",
                "end",
                "room",
                "grains",
                "topic",
                "req+pref");
    for (int i : order)
    {
        const MeetingAssignment &assignment = schedule.meetingAssignments[i];
        const Meeting &meeting = schedule.meetings[assignment.meeting];
        if (assignment.grainRoom < 0)
        {
            std::printf("  ---- unassigned: %s\n", meeting.topic.c_str());
            continue;
        }
        const int pos = schedule.timeGrainPosOf(assignment.grainRoom);
        const int room = schedule.roomIndexOf(assignment.grainRoom);
        const TimeGrain &grain = schedule.timeGrains[pos];
        const int endMinute
            = grain.startingMinuteOfDay
              + meeting.durationInGrains * kGrainLengthInMinutes;
        std::printf("  %-4d %-6s %-6s %-8s %-6d %-30s %zu+%zu\n",
                    grain.dayOfYear,
                    minuteToString(grain.startingMinuteOfDay).c_str(),
                    minuteToString(endMinute).c_str(),
                    schedule.rooms[room].name.c_str(),
                    meeting.durationInGrains,
                    meeting.topic.c_str(),
                    meeting.requiredAttendees.size(),
                    meeting.preferredAttendees.size());
    }
    std::printf("\n");
}

void printBreakdown(const Breakdown &b)
{
    std::printf("  [hard]   room conflict grains:        %lld\n",
                static_cast<long long>(b.roomConflictGrains));
    std::printf("  [hard]   overtime (last index sum):   %lld\n",
                static_cast<long long>(b.overtimeSum));
    std::printf("  [hard]   required conflict grains:    %lld\n",
                static_cast<long long>(b.requiredConflictGrains));
    std::printf("  [hard]   room capacity shortage:      %lld\n",
                static_cast<long long>(b.roomCapacityShortage));
    std::printf("  [hard]   start/end day violations:    %lld\n",
                static_cast<long long>(b.startEndDayViolations));
    std::printf("  [medium] required+preferred grains:   %lld\n",
                static_cast<long long>(b.requiredPreferredConflictGrains));
    std::printf("  [medium] preferred conflict grains:   %lld\n",
                static_cast<long long>(b.preferredConflictGrains));
    std::printf("  [soft]   last-grain-index sum (x1):   %lld\n",
                static_cast<long long>(b.lastGrainIndexSum));
    std::printf("  [soft]   consecutive pairs (x100):    %lld\n",
                static_cast<long long>(b.consecutiveMeetingPairs));
    std::printf("  [soft]   overlapping grains (x10):    %lld\n",
                static_cast<long long>(b.overlappingMeetingGrains));
    std::printf("  [soft]   larger-room capacity (x1):   %lld\n",
                static_cast<long long>(b.largerRoomCapacitySum));
    std::printf("  [soft]   room stability (x1):         %lld\n",
                static_cast<long long>(b.roomStabilityViolations));
    std::printf("\n");
}

}  // namespace

int main(int argc, char **argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;

    MeetingSchedule schedule = buildDemoData();
    std::printf("== meeting scheduling demo (%zu people, %zu time grains (4 days x 40), "
                "%zu rooms, %zu meetings, %d s) ==\n",
                schedule.people.size(),
                schedule.timeGrains.size(),
                schedule.rooms.size(),
                schedule.meetings.size(),
                seconds);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(seconds);
    config.randomSeed = 42;
    config.onNewBest = [](std::int64_t step, const Score &score)
    {
        std::printf("    new best @step %-8lld %s\n",
                    static_cast<long long>(step),
                    score.toString().c_str());
    };
    Solver solver(config);
    solver.solve(schedule);

    std::printf("\n  final score: %s  (feasible=%s)\n\n",
                schedule.score.toString().c_str(),
                schedule.score.isFeasible() ? "true" : "false");
    printSchedule(schedule);
    printBreakdown(ScoreCalculator::computeBreakdown(schedule));

    bool allAssigned = true;
    for (const MeetingAssignment &assignment : schedule.meetingAssignments)
    {
        if (assignment.grainRoom < 0)
            allAssigned = false;
    }
    std::printf("  all meetings assigned: %s\n", allAssigned ? "OK" : "FAIL");

    if (schedule.score.hard == 0 && allAssigned)
    {
        std::printf("  reached hard 0: OK\n");
        return 0;
    }
    std::printf("  failed to reach hard 0 (hard=%lld)\n",
                static_cast<long long>(schedule.score.hard));
    return 1;
}

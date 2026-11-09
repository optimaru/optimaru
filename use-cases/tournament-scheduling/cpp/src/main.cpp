// Tournament-scheduling demo: builds the demo tournament (7 teams, 18 days,
// 4 match slots per day, 12 unavailability penalties drawn from a seeded
// std::mt19937_64) and solves it,
// asserting a feasible (0hard) schedule.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <random>

#include "ts/domain.hpp"
#include "ts/score_calculator.hpp"
#include "ts/solver.hpp"
#include "optimaru/portable_random.hpp"

using namespace ts;

namespace
{

constexpr int DAY_COUNT = 18;
constexpr int ASSIGNMENTS_PER_DAY = 4;
constexpr int UNAVAILABILITY_PENALTY_COUNT = 12;

// Seeded, platform-independent draws (see optimaru/portable_random.hpp).
using DemoRandom = optimaru::PortableRandom;

// The demo instance: teams, days, penalties, assignments.
TournamentSchedule generateDemoData()
{
    DemoRandom random(0);
    TournamentSchedule schedule;
    schedule.teams = {{"Maarten"},
                      {"Geoffrey"},
                      {"Lukas"},
                      {"Chris"},
                      {"Fred"},
                      {"Radek"},
                      {"Maciej"}};
    for (int d = 0; d < DAY_COUNT; ++d)
        schedule.days.push_back({d});

    while (static_cast<int>(schedule.unavailabilityPenalties.size())
           < UNAVAILABILITY_PENALTY_COUNT)
    {
        const int team = random.next_int(schedule.teamCount());
        const int day = random.next_int(schedule.dayCount());
        bool duplicate = false;
        for (const UnavailabilityPenalty &penalty :
             schedule.unavailabilityPenalties)
        {
            if (penalty.team == team && penalty.day == day)
                duplicate = true;
        }
        if (!duplicate)
            schedule.unavailabilityPenalties.push_back({team, day});
    }

    std::int64_t id = 0;
    for (int d = 0; d < DAY_COUNT; ++d)
    {
        for (int i = 0; i < ASSIGNMENTS_PER_DAY; ++i)
        {
            TeamAssignment assignment;
            assignment.id = id++;
            assignment.day = d;
            assignment.indexInDay = i;
            schedule.teamAssignments.push_back(assignment);
        }
    }
    return schedule;
}

void printSchedule(const TournamentSchedule &schedule)
{
    const int teamCount = schedule.teamCount();
    std::vector<std::vector<char>> unavailable(
        teamCount, std::vector<char>(schedule.dayCount(), 0));
    for (const UnavailabilityPenalty &penalty :
         schedule.unavailabilityPenalties)
    {
        unavailable[penalty.team][penalty.day] = 1;
    }

    std::printf("  [fixtures per day]  (! = unavailability violated)\n");
    for (int d = 0; d < schedule.dayCount(); ++d)
    {
        std::printf("    Day %2d:", d);
        for (const TeamAssignment &assignment : schedule.teamAssignments)
        {
            if (assignment.day != d)
                continue;
            if (assignment.team < 0)
            {
                std::printf("  ----");
                continue;
            }
            std::printf("  %s%s",
                        schedule.teams[assignment.team].name.c_str(),
                        unavailable[assignment.team][d] ? "!" : "");
        }
        std::printf("\n");
    }

    std::printf("\n  [assignments per team]");
    std::vector<int> loads(teamCount, 0);
    for (const TeamAssignment &assignment : schedule.teamAssignments)
    {
        if (assignment.team >= 0)
            ++loads[assignment.team];
    }
    for (int t = 0; t < teamCount; ++t)
    {
        std::printf(" %s=%d", schedule.teams[t].name.c_str(), loads[t]);
    }

    std::printf("\n\n  [head-to-heads per team pair]\n");
    std::vector<std::vector<int>> perDay(
        teamCount, std::vector<int>(schedule.dayCount(), 0));
    for (const TeamAssignment &assignment : schedule.teamAssignments)
    {
        if (assignment.team >= 0)
            ++perDay[assignment.team][assignment.day];
    }
    for (int t1 = 0; t1 < teamCount; ++t1)
    {
        std::printf("    %-8s", schedule.teams[t1].name.c_str());
        for (int t2 = 0; t2 < teamCount; ++t2)
        {
            if (t2 <= t1)
            {
                std::printf("  .");
                continue;
            }
            int confrontations = 0;
            for (int d = 0; d < schedule.dayCount(); ++d)
                confrontations += perDay[t1][d] * perDay[t2][d];
            std::printf(" %2d", confrontations);
        }
        std::printf("\n");
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char **argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;

    TournamentSchedule schedule = generateDemoData();
    std::printf("== tournament scheduling demo (%d teams, %d days x %d slots = %zu assignments, "
                "%zu unavailable, %d s) ==\n",
                schedule.teamCount(),
                DAY_COUNT,
                ASSIGNMENTS_PER_DAY,
                schedule.teamAssignments.size(),
                schedule.unavailabilityPenalties.size(),
                seconds);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(seconds);
    config.seed = 42;
    config.on_new_best = [](std::int64_t step, const Score &score)
    {
        std::printf("    new best @step %-8lld %s\n",
                    static_cast<long long>(step),
                    score.str().c_str());
    };
    Solver solver(config);
    solver.solve(schedule);

    std::printf("\n  final score: %s  (feasible=%s)\n",
                schedule.score.str().c_str(),
                schedule.score.feasible() ? "true" : "false");
    // Decompose back into the Java score levels for readability.
    {
        std::vector<std::int64_t> loads;
        std::vector<std::int64_t> confrontations;
        std::vector<std::vector<int>> perDay(
            schedule.teamCount(), std::vector<int>(schedule.dayCount(), 0));
        for (const TeamAssignment &assignment : schedule.teamAssignments)
        {
            if (assignment.team >= 0)
                ++perDay[assignment.team][assignment.day];
        }
        for (int t = 0; t < schedule.teamCount(); ++t)
        {
            std::int64_t load = 0;
            for (int d = 0; d < schedule.dayCount(); ++d)
                load += perDay[t][d];
            if (load > 0)
                loads.push_back(load);
        }
        for (int t1 = 0; t1 < schedule.teamCount(); ++t1)
        {
            for (int t2 = t1 + 1; t2 < schedule.teamCount(); ++t2)
            {
                std::int64_t count = 0;
                for (int d = 0; d < schedule.dayCount(); ++d)
                    count += perDay[t1][d] * perDay[t2][d];
                if (count > 0)
                    confrontations.push_back(count);
            }
        }
        std::printf("  (at Java score levels: %lldhard/-%.6fmedium/-%.6fsoft)\n\n",
                    static_cast<long long>(schedule.score.hard),
                    ScoreCalculator::unfairness(loads),
                    ScoreCalculator::unfairness(confrontations));
    }
    printSchedule(schedule);

    bool allAssigned = true;
    for (const TeamAssignment &assignment : schedule.teamAssignments)
    {
        if (assignment.team < 0)
            allAssigned = false;
    }
    std::printf("  all assignments made: %s\n", allAssigned ? "OK" : "FAIL");

    if (schedule.score.hard == 0 && allAssigned)
    {
        std::printf("  reached hard 0 (feasible): OK\n");
        return 0;
    }
    std::printf("  failed to reach hard 0 (hard=%lld)\n",
                static_cast<long long>(schedule.score.hard));
    return 1;
}

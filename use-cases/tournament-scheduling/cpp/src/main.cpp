// Tournament-scheduling demo: reproduces the Java quickstart's
// DemoDataGenerator (7 teams, 18 days, 4 match slots per day, 12
// unavailability penalties drawn with java.util.Random(0)) and solves it,
// asserting a feasible (0hard) schedule.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ts/domain.hpp"
#include "ts/score_calculator.hpp"
#include "ts/solver.hpp"

using namespace ts;

namespace
{

constexpr int DAY_COUNT = 18;
constexpr int ASSIGNMENTS_PER_DAY = 4;
constexpr int UNAVAILABILITY_PENALTY_COUNT = 12;

// Faithful replica of java.util.Random (48-bit LCG), so the demo's
// unavailability penalties match the Java generator's Random(0) draw exactly.
class JavaRandom
{
public:
    explicit JavaRandom(std::int64_t seed)
        : seed_((static_cast<std::uint64_t>(seed) ^ kMultiplier) & kMask)
    {
    }

    int nextInt(int bound)
    {
        // bound 7 / 18 are not powers of two: the rejection path of
        // java.util.Random.nextInt(int).
        if ((bound & -bound) == bound)  // power of two
        {
            return static_cast<int>(
                (static_cast<std::int64_t>(bound) * next(31)) >> 31);
        }
        std::int64_t bits, val;
        do
        {
            bits = next(31);
            val = bits % bound;
            // Java retries when `bits - val + (bound - 1)` overflows int32.
        } while (bits - val + (bound - 1) >= (std::int64_t{1} << 31));
        return static_cast<int>(val);
    }

private:
    std::int64_t next(int bits)
    {
        seed_ = (seed_ * kMultiplier + kAddend) & kMask;
        return static_cast<std::int64_t>(seed_ >> (48 - bits));
    }

    static constexpr std::uint64_t kMultiplier = 0x5DEECE66DULL;
    static constexpr std::uint64_t kAddend = 0xBULL;
    static constexpr std::uint64_t kMask = (1ULL << 48) - 1;
    std::uint64_t seed_;
};

// The Java DemoDataGenerator, verbatim: teams, days, penalties, assignments.
TournamentSchedule generateDemoData()
{
    JavaRandom random(0);
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
        const int team = random.nextInt(schedule.teamCount());
        const int day = random.nextInt(schedule.dayCount());
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

    std::printf("\n  final score: %s  (feasible=%s)\n",
                schedule.score.toString().c_str(),
                schedule.score.isFeasible() ? "true" : "false");
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

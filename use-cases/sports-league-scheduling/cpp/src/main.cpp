// Demo: the demo league, modelled on the quickstart's demo — 14 South
// American teams, 32 rounds, 182 matches (every ordered pair), ~5% classic
// matches drawn from a seeded std::mt19937_64 plus the symmetric closure.
// Round 0 is pinned to a Wednesday so the instance is deterministic.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <random>

#include "sls/domain.hpp"
#include "sls/score_calculator.hpp"
#include "sls/solver.hpp"

using namespace sls;

namespace
{

// The demo generator's random draws: std::mt19937_64 with hand-written
// bounded draws, so a seed gives the same instance with every standard
// library (the std:: distributions are implementation-defined).
class DemoRandom
{
public:
    explicit DemoRandom(std::uint64_t seed)
        : engine_(seed)
    {
    }

    // Uniform in [0, bound), bound > 0.
    int next_int(int bound)
    {
        return static_cast<int>(engine_() % static_cast<std::uint64_t>(bound));
    }

    // Uniform in [0, bound), bound > 0.
    std::int64_t next_long(std::int64_t bound)
    {
        return static_cast<std::int64_t>(engine_()
                                         % static_cast<std::uint64_t>(bound));
    }

    // Uniform in [0, 1).
    double next_double()
    {
        return static_cast<double>(engine_() >> 11) * 0x1.0p-53;
    }

private:
    std::mt19937_64 engine_;
};

constexpr int ROUNDS = 32;
// Round 0 is a Wednesday (0=Mon .. 6=Sun), so the weekend rounds are fixed.
constexpr int FIRST_ROUND_DAY_OF_WEEK = 2;

const int DISTANCE_IN_KM[14][14] = {
    {0,
     2163,
     2163,
     2160,
     2156,
     2156,
     2163,
     340,
     1342,
     512,
     3038,
     1526,
     2054,
     2054},
    {2163, 0, 11, 50, 813, 813, 11, 1967, 842, 1661, 1139, 1037, 202, 202},
    {2163, 11, 0, 50, 813, 813, 11, 1967, 842, 1661, 1139, 1037, 202, 202},
    {2160, 50, 50, 0, 862, 862, 50, 1957, 831, 1655, 1180, 1068, 161, 161},
    {2160, 813, 813, 862, 0, 1, 813, 2083, 1160, 1741, 910, 644, 600, 600},
    {2160, 813, 813, 862, 1, 0, 813, 2083, 1160, 1741, 910, 644, 600, 600},
    {2163, 11, 11, 50, 813, 813, 0, 1967, 842, 1661, 1139, 1037, 202, 202},
    {340,
     1967,
     1967,
     1957,
     2083,
     2083,
     1967,
     0,
     1126,
     341,
     2926,
     1490,
     1836,
     1836},
    {1342, 842, 842, 831, 1160, 1160, 842, 1126, 0, 831, 1874, 820, 714, 714},
    {512,
     1661,
     1661,
     1655,
     1741,
     1741,
     1661,
     341,
     831,
     0,
     2589,
     1151,
     1545,
     1545},
    {3038,
     1139,
     1139,
     1180,
     910,
     910,
     1139,
     2926,
     1874,
     2589,
     0,
     1552,
     1340,
     1340},
    {1526,
     1037,
     1037,
     1068,
     644,
     644,
     1037,
     1490,
     820,
     1151,
     1552,
     0,
     1077,
     1077},
    {2054, 202, 202, 161, 600, 600, 202, 1836, 714, 1545, 1340, 1077, 0, 14},
    {2054, 202, 202, 161, 600, 600, 202, 1836, 714, 1545, 1340, 1077, 14, 0},
};

const char *TEAM_NAMES[14] = {"Cruzeiro",
                              "Argentinos Jr.",
                              "Boca Juniors",
                              "Estudiantes",
                              "Independente",
                              "Racing",
                              "River Plate",
                              "Flamengo",
                              "Gremio",
                              "Santos",
                              "Colo-Colo",
                              "Olimpia",
                              "Nacional",
                              "Penharol"};

LeagueSchedule generateDemoData()
{
    LeagueSchedule schedule;
    DemoRandom random(0);

    // Rounds: weekends flagged from the pinned first day of week.
    for (int i = 0; i < ROUNDS; ++i)
    {
        Round round;
        round.index = i;
        round.weekendOrHoliday = ((FIRST_ROUND_DAY_OF_WEEK + i) % 7) >= 5;
        schedule.rounds.push_back(round);
    }

    // Teams + distances.
    for (int i = 0; i < 14; ++i)
    {
        schedule.teams.push_back({std::to_string(i + 1), TEAM_NAMES[i]});
    }
    schedule.distanceToTeam.assign(14, std::vector<int>(14, 0));
    for (int i = 0; i < 14; ++i)
    {
        for (int j = 0; j < 14; ++j)
            schedule.distanceToTeam[i][j] = DISTANCE_IN_KM[i][j];
    }

    // Matches: every ordered pair (i, j), i != j — 182 of them.
    for (int i = 0; i < 14; ++i)
    {
        for (int j = 0; j < 14; ++j)
        {
            if (i == j)
                continue;
            Match match;
            match.id = schedule.teams[i].id + "-" + schedule.teams[j].id;
            match.homeTeam = i;
            match.awayTeam = j;
            schedule.matches.push_back(match);
        }
    }

    // 5% classic matches: pick the k-th non-classic match, k uniform in
    // [0, size), `size` shrinking by one per pick.
    const int classicPicks
        = static_cast<int>(schedule.matches.size() * 0.05);  // 9
    int size = static_cast<int>(schedule.matches.size());
    for (int pick = 0; pick < classicPicks && size > 0; ++pick, --size)
    {
        int skip = random.next_int(size);
        for (Match &match : schedule.matches)
        {
            if (match.classicMatch)
                continue;
            if (skip-- == 0)
            {
                match.classicMatch = true;
                break;
            }
        }
    }
    // Symmetric closure: the reversed fixture of a classic match is classic.
    for (Match &match : schedule.matches)
    {
        if (match.classicMatch)
            continue;
        for (const Match &other : schedule.matches)
        {
            if (other.classicMatch && other.homeTeam == match.awayTeam
                && other.awayTeam == match.homeTeam)
            {
                match.classicMatch = true;
                break;
            }
        }
    }
    return schedule;
}

void printSchedule(const LeagueSchedule &schedule)
{
    for (int r = 0; r < schedule.roundCount(); ++r)
    {
        std::printf("  round %2d%s:",
                    schedule.rounds[r].index,
                    schedule.rounds[r].weekendOrHoliday ? "*" : " ");
        for (const Match &match : schedule.matches)
        {
            if (match.round == r)
            {
                std::printf(" %s-%s%s",
                            schedule.teams[match.homeTeam].id.c_str(),
                            schedule.teams[match.awayTeam].id.c_str(),
                            match.classicMatch ? "!" : "");
            }
        }
        std::printf("\n");
    }
    std::printf("  (*: weekend/holiday round, !: classic match)\n\n");
}

}  // namespace

int main(int argc, char **argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;

    LeagueSchedule schedule = generateDemoData();
    int classicCount = 0;
    for (const Match &match : schedule.matches)
        classicCount += match.classicMatch ? 1 : 0;
    std::printf("== Sports league scheduling demo (%d teams, %zu matches, "
                "%d rounds, %d classic, %ds) ==\n",
                schedule.teamCount(),
                schedule.matches.size(),
                schedule.roundCount(),
                classicCount,
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

    std::printf("\n  final score: %s  (feasible=%s)\n\n",
                schedule.score.str().c_str(),
                schedule.score.feasible() ? "true" : "false");
    printSchedule(schedule);

    const Breakdown b = ScoreCalculator::computeBreakdown(schedule);
    std::printf("  hard: teamConflictCost=%lld "
                "consecutiveHomeMatchesCost=%lld "
                "consecutiveAwayMatchesCost=%lld "
                "repeatMatchOnNextDayCost=%lld\n",
                static_cast<long long>(b.teamConflictCost),
                static_cast<long long>(b.consecutiveHomeMatchesCost),
                static_cast<long long>(b.consecutiveAwayMatchesCost),
                static_cast<long long>(b.repeatMatchOnNextDayCost));
    std::printf("  soft: travel km startToAwayHopCost=%lld "
                "homeToAwayHopCost=%lld awayToAwayHopCost=%lld "
                "awayToHomeHopCost=%lld awayToEndHopCost=%lld, "
                "workdayClassicMatchesCost=%lld\n",
                static_cast<long long>(b.startToAwayHopCost),
                static_cast<long long>(b.homeToAwayHopCost),
                static_cast<long long>(b.awayToAwayHopCost),
                static_cast<long long>(b.awayToHomeHopCost),
                static_cast<long long>(b.awayToEndHopCost),
                static_cast<long long>(b.workdayClassicMatchesCost));

    bool allAssigned = true;
    for (const Match &match : schedule.matches)
    {
        if (match.round < 0)
            allAssigned = false;
    }
    std::printf("  every match assigned: %s\n", allAssigned ? "OK" : "FAIL");

    if (schedule.score.hard == 0 && allAssigned)
    {
        std::printf("  reached 0hard: OK\n");
        return 0;
    }
    std::printf("  FAILED to reach 0hard (hard=%lld)\n",
                static_cast<long long>(schedule.score.hard));
    return 1;
}

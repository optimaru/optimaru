// Demo: the quickstart's DemoDataGenerator reproduced in C++ — 14 South
// American teams, 32 rounds, 182 matches (every ordered pair), ~5% classic
// matches picked with java.util.Random(0) exactly like the Java code plus the
// symmetric closure. The only divergence: Java flags weekend rounds from
// LocalDate.now(); this demo pins round 0 to a fixed Wednesday so the
// instance is deterministic.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sls/domain.hpp"
#include "sls/score_calculator.hpp"
#include "sls/solver.hpp"

using namespace sls;

namespace
{

// java.util.Random (48-bit LCG), enough for nextInt(bound) — reproduces the
// quickstart's classic-match sampling bit for bit.
class JavaRandom
{
public:
    explicit JavaRandom(std::int64_t seed)
        : seed_((static_cast<std::uint64_t>(seed) ^ kMultiplier) & kMask)
    {
    }

    int nextInt(int bound)
    {
        if ((bound & -bound) == bound)  // power of two
        {
            return static_cast<int>(
                (static_cast<std::int64_t>(bound) * next(31)) >> 31);
        }
        int bits;
        int val;
        do
        {
            bits = next(31);
            val = bits % bound;
        } while (bits - val + (bound - 1) < 0);
        return val;
    }

private:
    static constexpr std::uint64_t kMultiplier = 0x5DEECE66DULL;
    static constexpr std::uint64_t kAddend = 0xBULL;
    static constexpr std::uint64_t kMask = (1ULL << 48) - 1;

    int next(int bits)
    {
        seed_ = (seed_ * kMultiplier + kAddend) & kMask;
        return static_cast<int>(seed_ >> (48 - bits));
    }

    std::uint64_t seed_;
};

constexpr int ROUNDS = 32;
// Java flags rounds landing on Saturday/Sunday of LocalDate.now(); the demo
// pins round 0 to Wednesday (0=Mon .. 6=Sun) for determinism.
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
    JavaRandom random(0);

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

    // 5% classic matches — the Java applyRandomValue loop with Random(0):
    // pick the nextInt(size)-th non-classic match, `size` shrinking by one
    // per pick.
    const int classicPicks
        = static_cast<int>(schedule.matches.size() * 0.05);  // 9
    int size = static_cast<int>(schedule.matches.size());
    for (int pick = 0; pick < classicPicks && size > 0; ++pick, --size)
    {
        int skip = random.nextInt(size);
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

    const Breakdown b = ScoreCalculator::computeBreakdown(schedule);
    std::printf("  hard: sameDay=%lld consecutiveHome=%lld "
                "consecutiveAway=%lld repeatNextDay=%lld\n",
                static_cast<long long>(b.matchesOnSameDay),
                static_cast<long long>(b.consecutiveHomeMatches),
                static_cast<long long>(b.consecutiveAwayMatches),
                static_cast<long long>(b.repeatMatchOnNextDay));
    std::printf("  soft: travel km start=%lld homeAway=%lld awayAway=%lld "
                "awayHome=%lld end=%lld, classic off-weekend=%lld\n",
                static_cast<long long>(b.startToAwayHop),
                static_cast<long long>(b.homeToAwayHop),
                static_cast<long long>(b.awayToAwayHop),
                static_cast<long long>(b.awayToHomeHop),
                static_cast<long long>(b.awayToEndHop),
                static_cast<long long>(b.classicMatchCount));

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

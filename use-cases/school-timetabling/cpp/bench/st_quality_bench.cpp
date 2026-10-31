// Quality benchmark driver for the C++ school-timetabling port.
//
// Track-1 comparison logic is the Java quickstart's SIX constraints
// (roomConflict / teacherConflict / studentGroupConflict hard,
// teacherRoomStability / teacherTimeEfficiency / studentGroupSubjectVariety
// soft). This repo's teacherUnavailable extension is deliberately left EMPTY
// in every generated instance so it contributes 0 and the comparison against
// the CP-SAT model stays fair.
//
// Two modes:
//   st_quality_bench <classes> <teachers> <rooms> <seed> <seconds> <out.json>
//                    [parallelSolverCount]
//     Generates a deterministic instance (5 days x 8 periods, fixed subject
//     curriculum, seed-shuffled subject->teacher split), dumps it as JSON
//     (the CP-SAT model and evaluate.py read the same file), solves for the
//     wall-clock budget and prints the trajectory +
//     "QBENCH_CPP_FINAL score=... moves_per_sec=N".
//     The best assignment is dumped next to the instance as
//     <out minus .json>.solution_cpp.json for the shared re-scorer.
//
//     Size presets (classes teachers rooms):
//       S:  6 12  7   (204 lessons,  40 slots x  7 rooms = 280 cells, 73%)
//       M: 12 20 13   (408 lessons,  40 slots x 13 rooms = 520 cells, 78%)
//       L: 24 40 25   (816 lessons,  40 slots x 25 rooms = 1000 cells, 82%)
//
//   st_quality_bench crosscheck <out.json>
//     Dumps a small FIXED instance with a FIXED assignment that exercises all
//     six constraints, and prints "CROSSCHECK_SCORE=..." — pairs with
//     benchmarks/school-timetabling/evaluate.py, which re-scores the same JSON.
//     The scores must be identical.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "st/domain.hpp"
#include "st/score_calculator.hpp"
#include "st/solver.hpp"

using namespace st;

namespace
{

constexpr int DAYS = 5;
constexpr int PERIODS_PER_DAY = 8;

struct SubjectHours
{
    const char *subject;
    int hours;
};

// Weekly hours per class; sums to 34 (6 free cells per class in the 40-slot
// week). ASCII ids keep the shared JSON trivially portable.
const std::vector<SubjectHours> CURRICULUM = {
    {"KOR", 5},
    {"MATH", 5},
    {"ENG", 4},
    {"SCI", 4},
    {"SOC", 4},
    {"PE", 3},
    {"MUS", 3},
    {"ART", 2},
    {"ETH", 2},
    {"HIS", 2},
};

// -------------------------------------------------------------------------
// Instance generation. Subject s is taught by the teacher pool
// {t : t % CURRICULUM.size() == s}; the classes are seed-shuffled and dealt
// round-robin over the pool, so the max load per teacher is
// ceil(classes / poolSize) * hours — comfortably under the 40-slot week for
// all presets, keeping the instances feasible but room-tight.
// -------------------------------------------------------------------------
Timetable
buildInstance(int classes, int teachers, int rooms, std::uint64_t seed)
{
    const int subjectCount = static_cast<int>(CURRICULUM.size());
    if (teachers < subjectCount)
    {
        std::fprintf(stderr,
                     "need at least %d teachers (one per subject)\n",
                     subjectCount);
        std::exit(2);
    }
    std::mt19937_64 rng(seed);

    Timetable t;
    for (int d = 0; d < DAYS; ++d)
    {
        for (int p = 1; p <= PERIODS_PER_DAY; ++p)
            t.timeslots.push_back({d, p});
    }
    for (int r = 0; r < rooms; ++r)
        t.rooms.push_back({"R" + std::to_string(r), "general"});

    std::vector<std::string> groups;
    for (int g = 0; g < classes; ++g)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "C%02d", g + 1);
        groups.emplace_back(buf);
    }

    // teacherOf[s][g] = teacher name for subject s in class g.
    std::vector<std::vector<std::string>> teacherOf(
        subjectCount, std::vector<std::string>(classes));
    for (int s = 0; s < subjectCount; ++s)
    {
        std::vector<int> pool;
        for (int tt = s; tt < teachers; tt += subjectCount)
            pool.push_back(tt);
        std::vector<int> order(classes);
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), rng);
        for (int i = 0; i < classes; ++i)
        {
            teacherOf[s][order[i]]
                = "T" + std::to_string(pool[i % pool.size()]);
        }
    }

    for (int g = 0; g < classes; ++g)
    {
        for (int s = 0; s < subjectCount; ++s)
        {
            for (int h = 1; h <= CURRICULUM[s].hours; ++h)
            {
                Lesson lesson;
                lesson.id = groups[g] + "-" + CURRICULUM[s].subject + "-"
                            + std::to_string(h);
                lesson.subject = CURRICULUM[s].subject;
                lesson.teacher = teacherOf[s][g];
                lesson.studentGroup = groups[g];
                t.lessons.push_back(std::move(lesson));
            }
        }
    }
    // NOTE: t.teacherUnavailable stays empty — see the file header.
    return t;
}

// -------------------------------------------------------------------------
// Fixed cross-check instance: 6 timeslots (2 days x 3 periods), 2 rooms,
// 7 lessons whose fixed assignment hits every constraint with a nonzero
// contribution. Expected score (see the pairwise counting in
// score_calculator.cpp): hard = -(1 room + 1 teacher + 1 group) = -3,
// soft = efficiency 3 - stability 4 - variety 2 = -3.
// -------------------------------------------------------------------------
Timetable buildCrossCheck()
{
    Timetable t;
    for (int d = 0; d < 2; ++d)
    {
        for (int p = 1; p <= 3; ++p)
            t.timeslots.push_back({d, p});
    }
    t.rooms.push_back({"R0", "general"});
    t.rooms.push_back({"R1", "general"});

    struct Fixed
    {
        const char *id, *subject, *teacher, *group;
        int timeslot, room;
    };
    const Fixed fixed[] = {
        {"L0", "MATH", "TA", "G1", 0, 0},
        {"L1", "MATH", "TA", "G1", 1, 0},
        {"L2", "PHYS", "TB", "G2", 0, 0},  // room conflict with L0
        {"L3", "PHYS", "TB", "G1", 0, 1},  // teacher (L2) + group (L0) conflict
        {"L4", "CHEM", "TB", "G2", 1, 1},
        {"L5", "MATH", "TA", "G2", 5, 1},
        {"L6", "MATH", "TC", "G2", 4, 0},
    };
    for (const Fixed &f : fixed)
    {
        Lesson lesson;
        lesson.id = f.id;
        lesson.subject = f.subject;
        lesson.teacher = f.teacher;
        lesson.studentGroup = f.group;
        lesson.slotRoom = f.timeslot * 2 + f.room;
        t.lessons.push_back(std::move(lesson));
    }
    return t;
}

// -------------------------------------------------------------------------
// JSON dumps (shared with the CP-SAT model and evaluate.py).
// -------------------------------------------------------------------------
void dumpInstance(const Timetable &t, const std::string &path)
{
    int days = 0;
    int periods = 0;
    for (const Timeslot &slot : t.timeslots)
    {
        days = std::max(days, slot.day + 1);
        periods = std::max(periods, slot.period);
    }
    std::ofstream out(path);
    out << "{\n  \"days\": " << days << ",\n  \"periodsPerDay\": " << periods
        << ",\n  \"timeslots\": [\n";
    for (int s = 0; s < t.timeslotCount(); ++s)
    {
        out << "    {\"day\": " << t.timeslots[s].day
            << ", \"period\": " << t.timeslots[s].period << "}"
            << (s + 1 < t.timeslotCount() ? "," : "") << '\n';
    }
    out << "  ],\n  \"rooms\": [";
    for (int r = 0; r < t.roomCount(); ++r)
        out << (r ? ", " : "") << '"' << t.rooms[r].name << '"';
    out << "],\n  \"lessons\": [\n";
    for (std::size_t i = 0; i < t.lessons.size(); ++i)
    {
        const Lesson &l = t.lessons[i];
        const int ts = l.slotRoom >= 0 ? t.timeslotIndexOf(l.slotRoom) : -1;
        const int room = l.slotRoom >= 0 ? t.roomIndexOf(l.slotRoom) : -1;
        out << "    {\"id\": \"" << l.id << "\", \"subject\": \"" << l.subject
            << "\", \"teacher\": \"" << l.teacher << "\", \"studentGroup\": \""
            << l.studentGroup << "\", \"timeslot\": " << ts
            << ", \"room\": " << room << "}"
            << (i + 1 < t.lessons.size() ? "," : "") << '\n';
    }
    out << "  ]\n}\n";
}

void dumpSolution(const Timetable &t, const std::string &path)
{
    std::ofstream out(path);
    out << "{\n  \"solver\": \"cpp\",\n  \"score\": \"" << t.score.toString()
        << "\",\n  \"assignment\": [\n";
    for (std::size_t i = 0; i < t.lessons.size(); ++i)
    {
        const Lesson &l = t.lessons[i];
        const int ts = l.slotRoom >= 0 ? t.timeslotIndexOf(l.slotRoom) : -1;
        const int room = l.slotRoom >= 0 ? t.roomIndexOf(l.slotRoom) : -1;
        out << "    {\"id\": \"" << l.id << "\", \"timeslot\": " << ts
            << ", \"room\": " << room << "}"
            << (i + 1 < t.lessons.size() ? "," : "") << '\n';
    }
    out << "  ]\n}\n";
}

std::string solutionPathFor(const std::string &instancePath)
{
    const std::string suffix = ".json";
    std::string base = instancePath;
    if (base.size() > suffix.size()
        && base.compare(base.size() - suffix.size(), suffix.size(), suffix)
               == 0)
    {
        base.erase(base.size() - suffix.size());
    }
    return base + ".solution_cpp.json";
}

int runCrossCheck(const std::string &outPath)
{
    Timetable t = buildCrossCheck();
    dumpInstance(t, outPath);
    ScoreCalculator calc;
    const Score score = calc.calculateScore(t);
    std::printf("CROSSCHECK_SCORE=%s\n", score.toString().c_str());
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc >= 3 && std::string(argv[1]) == "crosscheck")
        return runCrossCheck(argv[2]);
    if (argc < 7)
    {
        std::fprintf(
            stderr,
            "usage: st_quality_bench <classes> <teachers> <rooms> <seed> "
            "<seconds> <out.json> [parallelSolverCount]\n"
            "       st_quality_bench crosscheck <out.json>\n");
        return 2;
    }
    const int classes = std::atoi(argv[1]);
    const int teachers = std::atoi(argv[2]);
    const int rooms = std::atoi(argv[3]);
    const std::uint64_t seed = std::strtoull(argv[4], nullptr, 10);
    const int seconds = std::atoi(argv[5]);
    const std::string outPath = argv[6];

    Timetable table = buildInstance(classes, teachers, rooms, seed);
    dumpInstance(table, outPath);
    std::printf(
        "QBENCH_CPP instance classes=%d teachers=%d rooms=%d lessons=%zu "
        "timeslots=%d seed=%llu\n",
        classes,
        teachers,
        rooms,
        table.lessons.size(),
        table.timeslotCount(),
        static_cast<unsigned long long>(seed));

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(seconds);
    config.randomSeed = seed;
    if (argc >= 8)
        config.parallelSolverCount = std::max(1, std::atoi(argv[7]));
    // Optional knob overrides for A/B tuning sweeps (unset = solver
    // defaults, i.e. exactly the published bench configuration). These only
    // change SEARCH knobs, never scoring semantics.
    if (const char *v = std::getenv("ST_BENCH_ASMOVE_WEIGHT"))
        config.ruinRecreateAsMoveWeight = std::atof(v);
    if (const char *v = std::getenv("ST_BENCH_ASMOVE_MAXRUIN"))
        config.ruinRecreateAsMoveMaxRuin = std::atoi(v);
    if (const char *v = std::getenv("ST_BENCH_NEARBY_PROB"))
        config.sameGroupNearbyProbability = std::atof(v);
    if (const char *v = std::getenv("ST_BENCH_LA_SIZE"))
        config.lateAcceptanceSize = static_cast<std::size_t>(std::atoll(v));
    const auto t0 = std::chrono::steady_clock::now();
    // Sustained move-evaluation speed, estimated from the improvement
    // trajectory (each engine step evaluates one move) — comparable with the
    // "move evaluation speed (N/sec)" figure other local-search solvers report.
    long long lastStep = 0;
    long long lastMs = 0;
    bool feasibleSeen = false;
    config.onNewBest = [&t0, &lastStep, &lastMs, &feasibleSeen](
                           std::int64_t step, const Score &sc)
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        lastStep = step;
        lastMs = ms;
        if (!feasibleSeen && sc.hard >= 0)
        {
            feasibleSeen = true;
            std::printf("QBENCH_CPP first_feasible t=%lldms\n",
                        static_cast<long long>(ms));
        }
        std::printf("QBENCH_CPP best t=%lldms step=%lld score=%s\n",
                    static_cast<long long>(ms),
                    static_cast<long long>(step),
                    sc.toString().c_str());
    };
    Solver solver(config);
    solver.solve(table);

    dumpSolution(table, solutionPathFor(outPath));
    const double movesPerSec = lastMs > 0
                                   ? 1000.0 * static_cast<double>(lastStep)
                                         / static_cast<double>(lastMs)
                                   : 0.0;
    std::printf("QBENCH_CPP_FINAL score=%s feasible=%s moves_per_sec=%.0f\n",
                table.score.toString().c_str(),
                table.score.isFeasible() ? "true" : "false",
                movesPerSec);
    return 0;
}

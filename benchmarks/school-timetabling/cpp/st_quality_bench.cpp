// Quality benchmark driver for the C++ school-timetabling solver.
//
// Track-1 comparison logic is the quickstart model's SIX constraints
// (roomConflictCost / teacherConflictCost / classConflictCost hard,
// teacherRoomChangeCost / teacherTimeEfficiencyCost / classSubjectRepeatCost
// soft). The model's seventh term, teacherUnavailabilityCost (the
// teacherUnavailable map), is deliberately left EMPTY
// in every generated instance so it contributes 0 and the comparison against
// the CP-SAT model stays fair.
//
// The instances come from Python: school_timetabling_cpsat/gen_quickstart.py (in
// benchmarks/school-timetabling/cpsat) generates them and writes each one as
// <name>.json (for the CP-SAT model and evaluate.py) and <name>.sttxt (the same
// data as whitespace-separated tokens, read here; the format is in that
// module's docstring). school-timetabling-compare runs both steps.
//
// Two modes:
//   st_quality_bench <instance.sttxt> <seed> <seconds> [parallel_solver_count]
//     Solves the instance for the wall-clock budget and prints the
//     trajectory + "QBENCH_CPP_FINAL score=... moves_per_sec=N".
//     The best assignment is dumped next to the instance as
//     <instance minus .sttxt>.solution_cpp.json for the shared re-scorer.
//
//   st_quality_bench crosscheck <crosscheck.sttxt>
//     Scores the assignment the file carries (gen_quickstart's fixed
//     instance, which exercises all six constraints) and prints
//     "CROSSCHECK_SCORE=..." — pairs with school_timetabling_cpsat/evaluate.py,
//     which re-scores the same instance from its JSON.
//     The scores must be identical.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "st/domain.hpp"
#include "st/score_calculator.hpp"
#include "st/solver.hpp"

using namespace st;

namespace {

// -------------------------------------------------------------------------
// Instance file (gen_quickstart.to_text): "ST1", then the timeslots, rooms and
// lessons sections, each headed by its name and count. A lesson carries its
// assignment as timeslot and room indices, -1 -1 when unassigned.
// NOTE: t.teacherUnavailable stays empty — see the file header.
// -------------------------------------------------------------------------
Timetable loadInstance(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string magic;
    in >> magic;
    if (magic != "ST1") throw std::runtime_error("bad magic in " + path);

    Timetable t;
    std::string section;
    std::size_t count = 0;

    in >> section >> count;
    if (section != "timeslots") throw std::runtime_error("expected timeslots");
    t.timeslots.resize(count);
    for (Timeslot& slot : t.timeslots) in >> slot.day >> slot.period;

    in >> section >> count;
    if (section != "rooms") throw std::runtime_error("expected rooms");
    t.rooms.resize(count);
    for (Room& room : t.rooms) {
        in >> room.name;
        room.type = "general";
    }

    in >> section >> count;
    if (section != "lessons") throw std::runtime_error("expected lessons");
    t.lessons.resize(count);
    for (Lesson& lesson : t.lessons) {
        int timeslot = -1;
        int room = -1;
        in >> lesson.id >> lesson.subject >> lesson.teacher >> lesson.studentGroup >> timeslot >>
                room;
        lesson.slotRoom = timeslot >= 0 && room >= 0 ? t.slotRoomOf(timeslot, room) : -1;
    }
    if (!in) throw std::runtime_error("truncated instance file " + path);
    return t;
}

// -------------------------------------------------------------------------
// Solution dump (read by evaluate.py).
// -------------------------------------------------------------------------
void dumpSolution(const Timetable& t, const std::string& path) {
    std::ofstream out(path);
    out << "{\n  \"solver\": \"cpp\",\n  \"score\": \"" << t.score.str()
        << "\",\n  \"assignment\": [\n";
    for (std::size_t i = 0; i < t.lessons.size(); ++i) {
        const Lesson& l = t.lessons[i];
        const int ts = l.slotRoom >= 0 ? t.timeslotIndexOf(l.slotRoom) : -1;
        const int room = l.slotRoom >= 0 ? t.roomIndexOf(l.slotRoom) : -1;
        out << "    {\"id\": \"" << l.id << "\", \"timeslot\": " << ts << ", \"room\": " << room
            << "}" << (i + 1 < t.lessons.size() ? "," : "") << '\n';
    }
    out << "  ]\n}\n";
}

std::string solutionPathFor(const std::string& instancePath) {
    const std::string suffix = ".sttxt";
    std::string base = instancePath;
    if (base.size() > suffix.size() &&
        base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
        base.erase(base.size() - suffix.size());
    }
    return base + ".solution_cpp.json";
}

int runCrossCheck(const std::string& path) {
    Timetable t = loadInstance(path);
    ScoreCalculator calc;
    const Score score = calc.score(t);
    std::printf("CROSSCHECK_SCORE=%s\n", score.str().c_str());
    return 0;
}

int run(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "crosscheck") return runCrossCheck(argv[2]);
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: st_quality_bench <instance.sttxt> <seed> <seconds> "
                     "[parallel_solver_count]\n"
                     "       st_quality_bench crosscheck <crosscheck.sttxt>\n");
        return 2;
    }
    const std::string path = argv[1];
    const std::uint64_t seed = std::strtoull(argv[2], nullptr, 10);
    const int seconds = std::atoi(argv[3]);

    Timetable table = loadInstance(path);
    std::printf("QBENCH_CPP instance file=%s lessons=%zu timeslots=%d rooms=%d seed=%llu\n",
                path.c_str(), table.lessons.size(), table.timeslotCount(), table.roomCount(),
                static_cast<unsigned long long>(seed));

    Solver::Config config;
    config.time_limit = std::chrono::seconds(seconds);
    config.seed = seed;
    if (argc >= 5) config.parallel_solver_count = std::max(1, std::atoi(argv[4]));
    // Optional knob overrides for A/B tuning sweeps (unset = solver
    // defaults, i.e. exactly the published bench configuration). These only
    // change SEARCH knobs, never scoring semantics.
    if (const char* v = std::getenv("ST_BENCH_ASMOVE_WEIGHT"))
        config.ruin_recreate_as_move_weight = std::atof(v);
    if (const char* v = std::getenv("ST_BENCH_ASMOVE_MAXRUIN"))
        config.ruin_recreate_as_move_max_ruin = std::atoi(v);
    if (const char* v = std::getenv("ST_BENCH_NEARBY_PROB"))
        config.same_group_nearby_probability = std::atof(v);
    if (const char* v = std::getenv("ST_BENCH_LA_SIZE"))
        config.history_length = static_cast<std::size_t>(std::atoll(v));
    const auto t0 = std::chrono::steady_clock::now();
    // Sustained move-evaluation speed, estimated from the improvement
    // trajectory (each engine step evaluates one move) — comparable with the
    // "move evaluation speed (N/sec)" figure other local-search solvers report.
    long long lastStep = 0;
    long long lastMs = 0;
    bool feasibleSeen = false;
    config.on_new_best = [&t0, &lastStep, &lastMs, &feasibleSeen](std::int64_t step,
                                                                  const Score& sc) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
        lastStep = step;
        lastMs = ms;
        if (!feasibleSeen && sc.hard >= 0) {
            feasibleSeen = true;
            std::printf("QBENCH_CPP first_feasible t=%lldms\n", static_cast<long long>(ms));
        }
        std::printf("QBENCH_CPP best t=%lldms step=%lld score=%s\n", static_cast<long long>(ms),
                    static_cast<long long>(step), sc.str().c_str());
    };
    Solver solver(config);
    solver.solve(table);

    dumpSolution(table, solutionPathFor(path));
    const double movesPerSec =
            lastMs > 0 ? 1000.0 * static_cast<double>(lastStep) / static_cast<double>(lastMs) : 0.0;
    std::printf("QBENCH_CPP_FINAL score=%s feasible=%s moves_per_sec=%.0f\n",
                table.score.str().c_str(), table.score.feasible() ? "true" : "false", movesPerSec);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "st_quality_bench: %s\n", error.what());
        return 1;
    }
}

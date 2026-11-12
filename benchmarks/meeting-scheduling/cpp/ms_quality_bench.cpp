// Quality-benchmark driver for the C++ meeting-scheduling use case.
//
// Solves a dumped comparison instance (benchmarks/meeting-scheduling/dump_instances.py
// writes the instances of the meeting-scheduling-compare harness, the demo
// data among them, as <name>.mstxt; the format is in that script's docstring)
// for a fixed wall-clock budget, then prints the per-constraint breakdown and
// a trajectory-based moves/s estimate (the st_quality_bench convention). The
// bench has no generator: every instance comes from Python, so a tuning run
// and the CP-SAT comparison solve the same problems.
//
// Usage: ms_quality_bench <instance.mstxt> <seed> <seconds> [k=v ...]
//        ms_quality_bench crosscheck <instance.mstxt>
//   crosscheck: no solve. Scores the fixed assignment i -> grainRoom
//   (7 * i) % grainRoomCount (a pinned assignment keeps its place) and prints
//   "MS_BENCH_CROSSCHECK hard=... soft=...", which must equal the score
//   dump_instances.py printed for that instance (its --check runs this).
// Knobs (bench-only overrides of Solver::Config, for A/B sweeps):
//   la= change= swap= alnsTrigger= alnsPolish= minRuin= maxRuin=
//   max_ruin_count= decay= dayFit= asMove= asMoveRuin=

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>

#include "ms/domain.hpp"
#include "ms/score_calculator.hpp"
#include "ms/solver.hpp"

using namespace ms;

namespace {

// ---------------------------------------------------------------------------
// Instance file (dump_instances.py). Strings are std::quoted; a list of ints
// is its length and then its items.
// ---------------------------------------------------------------------------
// A whole non-negative number, or an error that names the argument.
long long parseCount(const char* text, const char* name) {
    char* end = nullptr;
    const long long value = std::strtoll(text, &end, 10);
    if (end == text || *end != '\0' || value < 0)
        throw std::runtime_error(std::string(name) + " must be a whole number, got '" + text + "'");
    return value;
}

void checkIndex(long long value, long long count, const char* what) {
    if (value < 0 || value >= count)
        throw std::runtime_error(std::string(what) + " " + std::to_string(value) +
                                 " is out of range 0.." + std::to_string(count - 1));
}

std::string readString(std::istream& in) {
    std::string text;
    in >> std::quoted(text);
    return text;
}

std::vector<int> readInts(std::istream& in) {
    std::size_t count = 0;
    in >> count;
    std::vector<int> values(in ? count : 0);
    for (int& value : values) in >> value;
    return values;
}

std::size_t readSection(std::istream& in, const char* name) {
    std::string section;
    std::size_t count = 0;
    in >> section >> count;
    if (!in || section != name) throw std::runtime_error(std::string("expected ") + name);
    return count;
}

MeetingSchedule loadSchedule(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string magic;
    in >> magic;
    if (magic != "MS1") throw std::runtime_error("bad magic in " + path);

    MeetingSchedule schedule;
    schedule.people.resize(readSection(in, "people"));
    for (Person& person : schedule.people) {
        person.id = readString(in);
        person.fullName = readString(in);
    }

    schedule.timeGrains.resize(readSection(in, "timeGrains"));
    for (TimeGrain& grain : schedule.timeGrains) {
        grain.id = readString(in);
        in >> grain.grainIndex >> grain.dayOfYear >> grain.startingMinuteOfDay;
    }

    schedule.rooms.resize(readSection(in, "rooms"));
    for (Room& room : schedule.rooms) {
        room.id = readString(in);
        room.name = readString(in);
        in >> room.capacity;
    }

    schedule.meetings.resize(readSection(in, "meetings"));
    for (Meeting& meeting : schedule.meetings) {
        meeting.id = readString(in);
        meeting.topic = readString(in);
        in >> meeting.durationInGrains;
        meeting.requiredAttendees = readInts(in);
        meeting.preferredAttendees = readInts(in);
        int entireGroup = 0;
        in >> entireGroup;
        meeting.entireGroupMeeting = entireGroup != 0;
    }

    schedule.meetingAssignments.resize(readSection(in, "meetingAssignments"));
    for (MeetingAssignment& assignment : schedule.meetingAssignments) {
        assignment.id = readString(in);
        int pinned = 0;
        in >> assignment.meeting >> assignment.grainRoom >> pinned;
        assignment.pinned = pinned != 0;
    }
    if (!in) throw std::runtime_error("truncated instance file " + path);

    // The file is read without the Python model's checks: refuse an index
    // that points outside its table before the scorer reads through it.
    const int personCount = static_cast<int>(schedule.people.size());
    const int meetingCount = static_cast<int>(schedule.meetings.size());
    for (const Meeting& meeting : schedule.meetings) {
        for (int person : meeting.requiredAttendees)
            checkIndex(person, personCount, "required attendee");
        for (int person : meeting.preferredAttendees)
            checkIndex(person, personCount, "preferred attendee");
    }
    for (const MeetingAssignment& assignment : schedule.meetingAssignments) {
        checkIndex(assignment.meeting, meetingCount, "assignment meeting");
        if (assignment.grainRoom != -1)
            checkIndex(assignment.grainRoom, schedule.grainRoomCount(), "grainRoom");
    }
    return schedule;
}

void printBreakdown(const Breakdown& b) {
    struct Row {
        const char* name;
        std::int64_t units;
    };
    const Row rows[] = {
            {"H.roomConflictCost", b.roomConflictCost},
            {"H.overtimeCost", b.overtimeCost},
            {"H.requiredAttendanceConflictCost", b.requiredAttendanceConflictCost},
            {"H.roomCapacityCost", b.roomCapacityCost},
            {"H.dayCrossingCost", b.dayCrossingCost},
            {"M.requiredAndPreferredAttendanceConflictCost",
             b.requiredAndPreferredAttendanceConflictCost},
            {"M.preferredAttendanceConflictCost", b.preferredAttendanceConflictCost},
            {"S.meetingEndTimeCost", b.meetingEndTimeCost},
            {"S.consecutiveMeetingsCost", b.consecutiveMeetingsCost},
            {"S.meetingOverlapCost", b.meetingOverlapCost},
            {"S.largerRoomCapacityCost", b.largerRoomCapacityCost},
            {"S.roomChangeCost", b.roomChangeCost},
    };
    for (const Row& row : rows) {
        if (row.units != 0)
            std::printf("MS_BENCH breakdown %s units=%lld\n", row.name,
                        static_cast<long long>(row.units));
    }
}

int run(int argc, char** argv) {
    const bool crosscheck = argc == 3 && std::string(argv[1]) == "crosscheck";
    if (!crosscheck && argc < 4) {
        std::fprintf(stderr,
                     "usage: ms_quality_bench <instance.mstxt> <seed> "
                     "<seconds> [key=value ...]\n"
                     "       ms_quality_bench crosscheck <instance.mstxt>\n");
        return 2;
    }

    // crosscheck: no solve. Scores a fixed assignment so the dumped instance
    // can be checked against the Python scorer (dump_instances.py prints the
    // same score).
    if (crosscheck) {
        MeetingSchedule schedule = loadSchedule(argv[2]);
        const int assignmentCount = static_cast<int>(schedule.meetingAssignments.size());
        for (int i = 0; i < assignmentCount; ++i) {
            MeetingAssignment& assignment = schedule.meetingAssignments[i];
            if (!assignment.pinned) assignment.grainRoom = (7 * i) % schedule.grainRoomCount();
        }
        printBreakdown(ScoreCalculator::computeBreakdown(schedule));
        const Score score = ScoreCalculator::score(schedule);
        std::printf("MS_BENCH_CROSSCHECK hard=%lld soft=%.0f\n", static_cast<long long>(score.hard),
                    score.soft);
        return 0;
    }

    const std::string path = argv[1];
    const std::uint64_t seed = static_cast<std::uint64_t>(parseCount(argv[2], "seed"));
    const int seconds = static_cast<int>(parseCount(argv[3], "seconds"));
    if (seconds == 0) throw std::runtime_error("seconds must be at least 1");
    MeetingSchedule schedule = loadSchedule(path);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(seconds);
    config.seed = seed;
    config.parallel_solver_count = 1;
    for (int a = 4; a < argc; ++a) {
        const char* eq = std::strchr(argv[a], '=');
        if (!eq) {
            std::fprintf(stderr, "bad knob %s\n", argv[a]);
            return 2;
        }
        const std::string key(argv[a], eq - argv[a]);
        const double value = std::atof(eq + 1);
        if (key == "la")
            config.history_length = static_cast<std::size_t>(value);
        else if (key == "change")
            config.change_weight = value;
        else if (key == "swap")
            config.swap_weight = value;
        else if (key == "alnsTrigger")
            config.ruin_recreate_unimproved_step_trigger = static_cast<std::int64_t>(value);
        else if (key == "alnsPolish")
            config.ruin_recreate_polish_steps = static_cast<std::int64_t>(value);
        else if (key == "minRuin")
            config.ruin_recreate_min_ruin_fraction = value;
        else if (key == "maxRuin")
            config.ruin_recreate_max_ruin_fraction = value;
        else if (key == "maxRuinCount")
            config.ruin_recreate_max_ruin_count = static_cast<int>(value);
        else if (key == "decay")
            config.ruin_recreate_operator_decay = value;
        else if (key == "dayFit")
            config.day_fit_value_probability = value;
        else if (key == "asMove")
            config.ruin_recreate_as_move_weight = value;
        else if (key == "asMoveRuin")
            config.ruin_recreate_as_move_max_ruin = static_cast<int>(value);
        else {
            std::fprintf(stderr, "unknown knob %s\n", key.c_str());
            return 2;
        }
    }

    std::printf(
            "MS_BENCH instance file=%s meetings=%zu people=%zu grains=%zu "
            "rooms=%zu seed=%llu budget=%ds\n",
            path.c_str(), schedule.meetings.size(), schedule.people.size(),
            schedule.timeGrains.size(), schedule.rooms.size(),
            static_cast<unsigned long long>(seed), seconds);

    const auto t0 = std::chrono::steady_clock::now();
    long long lastStep = 0;
    long long lastMs = 0;
    long long feasibleMs = -1;
    config.on_new_best = [&t0, &lastStep, &lastMs, &feasibleMs](std::int64_t step,
                                                                const Score& sc) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
        lastStep = step;
        lastMs = ms;
        if (feasibleMs < 0 && sc.hard >= 0) feasibleMs = ms;
    };

    Solver solver(config);
    solver.solve(schedule);

    const double movesPerSec =
            lastMs > 0 ? 1000.0 * static_cast<double>(lastStep) / static_cast<double>(lastMs) : 0.0;
    Breakdown breakdown = ScoreCalculator::computeBreakdown(schedule);
    printBreakdown(breakdown);
    std::printf(
            "MS_BENCH_FINAL hard=%lld medium=%lld soft=%lld feasible=%s "
            "first_feasible_ms=%lld moves_per_sec=%.0f\n",
            static_cast<long long>(breakdown.hardTotal()),
            static_cast<long long>(breakdown.mediumTotal()),
            static_cast<long long>(breakdown.softTotal()),
            schedule.score.feasible() ? "true" : "false", feasibleMs, movesPerSec);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ms_quality_bench: %s\n", error.what());
        return 1;
    }
}

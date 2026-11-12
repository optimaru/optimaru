// Quality-benchmark driver for the C++ conference-scheduling solver.
//
// Solves a dumped comparison instance (benchmarks/conference-scheduling/dump_instances.py
// writes the instances of the conference-scheduling-compare harness, the demo
// conference among them, as <name>.cstxt; the format is in that script's
// docstring) for a fixed wall-clock budget, then prints the best-score
// trajectory, the per-constraint breakdown and an st_quality_bench-style
// moves/s estimate. The bench has no generator: every instance comes from
// Python, so a tuning run and the CP-SAT comparison solve the same problems.
//
// Usage: cs_quality_bench <instance.cstxt> <seed> <seconds> [key=value..]
//        cs_quality_bench crosscheck <instance.cstxt>
//   crosscheck: no solve. Scores the fixed assignment talk i -> slotRoom
//   (7 * i) % slotRoomCount (a pinned talk keeps its place) and prints
//   "CS_BENCH_CROSSCHECK hard=... soft=...", which must equal the score
//   dump_instances.py printed for that instance (its --check runs this).
// Knobs (bench-only overrides of Solver::Config, for A/B sweeps):
//   compatProb= la= change= swap= alnsTrigger= alnsPolish= minRuin= maxRuin=
//   max_ruin_count= decay= segments= asMove= as_move_max_ruin= descent=

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

#include "cs/domain.hpp"
#include "cs/score_calculator.hpp"
#include "cs/solver.hpp"

using namespace cs;

namespace {

// ---------------------------------------------------------------------------
// Instance file (dump_instances.py). Strings are std::quoted; a list is its
// length and then its items.
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

std::vector<std::string> readStrings(std::istream& in) {
    std::size_t count = 0;
    in >> count;
    std::vector<std::string> values(in ? count : 0);
    for (std::string& value : values) value = readString(in);
    return values;
}

std::size_t readSection(std::istream& in, const char* name) {
    std::string section;
    std::size_t count = 0;
    in >> section >> count;
    if (!in || section != name) throw std::runtime_error(std::string("expected ") + name);
    return count;
}

// The eight preference tag sets Talk and Speaker share, in the file's order.
template <typename T>
void readPreferenceTags(std::istream& in, T& owner) {
    owner.requiredTimeslotTags = readStrings(in);
    owner.preferredTimeslotTags = readStrings(in);
    owner.prohibitedTimeslotTags = readStrings(in);
    owner.undesiredTimeslotTags = readStrings(in);
    owner.requiredRoomTags = readStrings(in);
    owner.preferredRoomTags = readStrings(in);
    owner.prohibitedRoomTags = readStrings(in);
    owner.undesiredRoomTags = readStrings(in);
}

ConferenceSchedule loadConference(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string token;
    in >> token;
    if (token != "CS1") throw std::runtime_error("bad magic in " + path);

    ConferenceSchedule c;
    in >> token;
    if (token != "name") throw std::runtime_error("expected name");
    c.name = readString(in);
    in >> token >> c.minimumConsecutiveTalksPauseInMinutes;
    if (token != "minimumConsecutiveTalksPauseInMinutes")
        throw std::runtime_error("expected minimumConsecutiveTalksPauseInMinutes");
    in >> token;
    if (token != "talkTypes") throw std::runtime_error("expected talkTypes");
    c.talkTypes = readStrings(in);

    c.timeslots.resize(readSection(in, "timeslots"));
    for (Timeslot& slot : c.timeslots) {
        slot.id = readString(in);
        in >> slot.startMinute >> slot.endMinute;
        slot.talkTypes = readInts(in);
        slot.tags = readStrings(in);
    }

    c.rooms.resize(readSection(in, "rooms"));
    for (Room& room : c.rooms) {
        room.id = readString(in);
        room.name = readString(in);
        in >> room.capacity;
        room.talkTypes = readInts(in);
        room.unavailableTimeslots = readInts(in);
        room.tags = readStrings(in);
    }

    c.speakers.resize(readSection(in, "speakers"));
    for (Speaker& speaker : c.speakers) {
        speaker.id = readString(in);
        speaker.name = readString(in);
        speaker.unavailableTimeslots = readInts(in);
        readPreferenceTags(in, speaker);
    }

    c.talks.resize(readSection(in, "talks"));
    for (Talk& talk : c.talks) {
        talk.code = readString(in);
        talk.title = readString(in);
        in >> talk.talkType;
        talk.speakers = readInts(in);
        talk.themeTrackTags = readStrings(in);
        talk.sectorTags = readStrings(in);
        talk.audienceTypes = readStrings(in);
        in >> talk.audienceLevel;
        talk.contentTags = readStrings(in);
        talk.language = readString(in);
        readPreferenceTags(in, talk);
        talk.mutuallyExclusiveTalksTags = readStrings(in);
        talk.prerequisiteTalks = readInts(in);
        int pinned = 0;
        in >> talk.favoriteCount >> talk.crowdControlRisk >> talk.slotRoom >> pinned;
        talk.pinned = pinned != 0;
    }
    if (!in) throw std::runtime_error("truncated instance file " + path);

    // The file is read without the Python model's checks: refuse an index
    // that points outside its table before the scorer reads through it.
    const int typeCount = static_cast<int>(c.talkTypes.size());
    const int speakerCount = static_cast<int>(c.speakers.size());
    for (const Timeslot& slot : c.timeslots)
        for (int type : slot.talkTypes) checkIndex(type, typeCount, "timeslot talk type");
    for (const Room& room : c.rooms) {
        for (int type : room.talkTypes) checkIndex(type, typeCount, "room talk type");
        for (int slot : room.unavailableTimeslots)
            checkIndex(slot, c.timeslotCount(), "room unavailable timeslot");
    }
    for (const Speaker& speaker : c.speakers)
        for (int slot : speaker.unavailableTimeslots)
            checkIndex(slot, c.timeslotCount(), "speaker unavailable timeslot");
    for (const Talk& talk : c.talks) {
        checkIndex(talk.talkType, typeCount, "talk type");
        for (int speaker : talk.speakers) checkIndex(speaker, speakerCount, "talk speaker");
        for (int other : talk.prerequisiteTalks) checkIndex(other, c.talkCount(), "prerequisite");
        if (talk.slotRoom != -1) checkIndex(talk.slotRoom, c.slotRoomCount(), "slotRoom");
    }
    return c;
}

void printBreakdown(const Breakdown& b) {
    struct Row {
        const char* name;
        std::int64_t units;
    };
    const Row rows[] = {
            {"H.talkTypeIncompatibilityCost", b.talkTypeIncompatibilityCost},
            {"H.roomUnavailabilityCost", b.roomUnavailabilityCost},
            {"H.roomConflictCost", b.roomConflictCost},
            {"H.speakerUnavailabilityCost", b.speakerUnavailabilityCost},
            {"H.speakerConflictCost", b.speakerConflictCost},
            {"H.talkPrerequisiteTalksCost", b.talkPrerequisiteTalksCost},
            {"H.talkMutuallyExclusiveTalksTagsCost", b.talkMutuallyExclusiveTalksTagsCost},
            {"H.consecutiveTalksPauseCost", b.consecutiveTalksPauseCost},
            {"H.crowdControlRiskCost", b.crowdControlRiskCost},
            {"H.speakerRequiredTimeslotTagsCost", b.speakerRequiredTimeslotTagsCost},
            {"H.speakerProhibitedTimeslotTagsCost", b.speakerProhibitedTimeslotTagsCost},
            {"H.talkRequiredTimeslotTagsCost", b.talkRequiredTimeslotTagsCost},
            {"H.talkProhibitedTimeslotTagsCost", b.talkProhibitedTimeslotTagsCost},
            {"H.speakerRequiredRoomTagsCost", b.speakerRequiredRoomTagsCost},
            {"H.speakerProhibitedRoomTagsCost", b.speakerProhibitedRoomTagsCost},
            {"H.talkRequiredRoomTagsCost", b.talkRequiredRoomTagsCost},
            {"H.talkProhibitedRoomTagsCost", b.talkProhibitedRoomTagsCost},
            {"S.themeTrackConflictCost", b.themeTrackConflictCost},
            {"S.themeTrackRoomChangeCost", b.themeTrackRoomChangeCost},
            {"S.sectorConflictCost", b.sectorConflictCost},
            {"S.audienceTypeThemeTrackConflictCost", b.audienceTypeThemeTrackConflictCost},
            {"S.contentAudienceLevelFlowViolationCost", b.contentAudienceLevelFlowViolationCost},
            {"S.contentConflictCost", b.contentConflictCost},
            {"S.differentDayTalksCost", b.differentDayTalksCost},
            {"S.popularTalksRoomSizeCost", b.popularTalksRoomSizeCost},
            {"S.speakerPreferredTimeslotTagsCost", b.speakerPreferredTimeslotTagsCost},
            {"S.speakerUndesiredTimeslotTagsCost", b.speakerUndesiredTimeslotTagsCost},
            {"S.talkPreferredTimeslotTagsCost", b.talkPreferredTimeslotTagsCost},
            {"S.talkUndesiredTimeslotTagsCost", b.talkUndesiredTimeslotTagsCost},
            {"S.speakerPreferredRoomTagsCost", b.speakerPreferredRoomTagsCost},
            {"S.speakerUndesiredRoomTagsCost", b.speakerUndesiredRoomTagsCost},
            {"S.talkPreferredRoomTagsCost", b.talkPreferredRoomTagsCost},
            {"S.talkUndesiredRoomTagsCost", b.talkUndesiredRoomTagsCost},
            {"S.speakerMakespanCost", b.speakerMakespanCost},
            {"R.audienceTypeDiversityCost", b.audienceTypeDiversityCost},
            {"R.audienceLevelDiversityCost", b.audienceLevelDiversityCost},
            {"R.languageDiversityCost", b.languageDiversityCost},
    };
    for (const Row& row : rows) {
        if (row.units != 0)
            std::printf("CS_BENCH breakdown %s units=%lld\n", row.name,
                        static_cast<long long>(row.units));
    }
}

int run(int argc, char** argv) {
    const bool crosscheck = argc == 3 && std::string(argv[1]) == "crosscheck";
    if (!crosscheck && argc < 4) {
        std::fprintf(stderr,
                     "usage: cs_quality_bench <instance.cstxt> <seed> "
                     "<seconds> [key=value ...]\n"
                     "       cs_quality_bench crosscheck <instance.cstxt>\n");
        return 2;
    }

    // crosscheck: no solve. Scores a fixed assignment so the dumped instance
    // can be checked against the Python scorer (dump_instances.py prints the
    // same score).
    if (crosscheck) {
        ConferenceSchedule conference = loadConference(argv[2]);
        for (int i = 0; i < conference.talkCount(); ++i) {
            Talk& talk = conference.talks[i];
            if (!talk.pinned) talk.slotRoom = (7 * i) % conference.slotRoomCount();
        }
        const Breakdown fixed = ScoreCalculator::computeBreakdown(conference);
        printBreakdown(fixed);
        const Score score = ScoreCalculator::score(conference);
        std::printf("CS_BENCH_CROSSCHECK hard=%lld soft=%.0f\n", static_cast<long long>(score.hard),
                    score.soft);
        return 0;
    }

    const std::string path = argv[1];
    const std::uint64_t seed = static_cast<std::uint64_t>(parseCount(argv[2], "seed"));
    const int seconds = static_cast<int>(parseCount(argv[3], "seconds"));
    if (seconds == 0) throw std::runtime_error("seconds must be at least 1");
    ConferenceSchedule conference = loadConference(path);

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
        if (key == "compatProb")
            config.compatible_value_probability = value;
        else if (key == "la")
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
        else if (key == "segments")
            config.restart_segments = static_cast<int>(value);
        else if (key == "asMove")
            config.ruin_recreate_as_move_weight = value;
        else if (key == "asMoveMaxRuin")
            config.ruin_recreate_as_move_max_ruin = static_cast<int>(value);
        else if (key == "descent")
            config.descent_weight = value;
        else if (key == "hot")
            config.hot_talk_probability = value;
        else {
            std::fprintf(stderr, "unknown knob %s\n", key.c_str());
            return 2;
        }
    }

    std::printf(
            "CS_BENCH instance file=%s talks=%zu timeslots=%zu rooms=%zu "
            "speakers=%zu seed=%llu budget=%ds\n",
            path.c_str(), conference.talks.size(), conference.timeslots.size(),
            conference.rooms.size(), conference.speakers.size(),
            static_cast<unsigned long long>(seed), seconds);

    const auto t0 = std::chrono::steady_clock::now();
    // Sustained move speed, estimated from the improvement trajectory (each
    // engine step evaluates one move) — the st_quality_bench convention.
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
        std::printf("CS_BENCH best t=%lldms step=%lld score=%s\n", static_cast<long long>(ms),
                    static_cast<long long>(step), sc.str().c_str());
    };

    Solver solver(config);
    solver.solve(conference);

    const double movesPerSec =
            lastMs > 0 ? 1000.0 * static_cast<double>(lastStep) / static_cast<double>(lastMs) : 0.0;
    Breakdown breakdown = ScoreCalculator::computeBreakdown(conference);
    printBreakdown(breakdown);
    std::printf(
            "CS_BENCH_FINAL hard=%lld soft=%.0f feasible=%s "
            "first_feasible_ms=%lld moves_per_sec=%.0f\n",
            static_cast<long long>(conference.score.hard), conference.score.soft,
            conference.score.feasible() ? "true" : "false", feasibleMs, movesPerSec);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cs_quality_bench: %s\n", error.what());
        return 1;
    }
}

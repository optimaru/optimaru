// Quality-benchmark driver for the C++ conference-scheduling port.
//
// Solves either (a) the demo instance (an exact copy of src/main.cpp's
// generator, java.util.Random draws included) or (b) a deterministic scaled
// instance in the demo's shape (single-typed slots/rooms, so the synthetic
// talkTypeCompatibility hard constraint dominates exactly like in the demo)
// for a fixed wall-clock budget, then prints the best-score trajectory, the
// per-constraint breakdown and an st_quality_bench-style moves/s estimate.
//
// Usage: cs_quality_bench <demo|scaled:<talks>> <seed> <seconds> [key=value..]
// Knobs (bench-only overrides of Solver::Config, for A/B sweeps):
//   compatProb= la= change= swap= alnsTrigger= alnsPolish= minRuin= maxRuin=
//   maxRuinCount= decay= segments= asMove= asMoveMaxRuin= descent=

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "cs/domain.hpp"
#include "cs/score_calculator.hpp"
#include "cs/solver.hpp"

using namespace cs;

namespace
{

// ---------------------------------------------------------------------------
// java.util.Random (48-bit LCG) — copy of src/main.cpp's, so the "demo"
// instance is bit-identical to the cs_demo binary's.
// ---------------------------------------------------------------------------
class JavaRandom
{
public:
    explicit JavaRandom(std::uint64_t seed)
        : seed_((seed ^ 0x5DEECE66DULL) & MASK)
    {
    }

    int nextInt(int bound)
    {
        int r = next(31);
        const int m = bound - 1;
        if ((bound & m) == 0)  // power of two
        {
            r = static_cast<int>((static_cast<std::int64_t>(bound) * r) >> 31);
        }
        else
        {
            int u = r;
            r = u % bound;
            while (static_cast<std::int32_t>(static_cast<std::uint32_t>(u)
                                             - static_cast<std::uint32_t>(r)
                                             + static_cast<std::uint32_t>(m))
                   < 0)
            {
                u = next(31);
                r = u % bound;
            }
        }
        return r;
    }

private:
    static constexpr std::uint64_t MASK = (1ULL << 48) - 1;
    std::uint64_t seed_;

    int next(int bits)
    {
        seed_ = (seed_ * 0x5DEECE66DULL + 0xBULL) & MASK;
        return static_cast<int>(seed_ >> (48 - bits));
    }
};

constexpr int LAB = 0;
constexpr int BREAKOUT = 1;

const std::vector<std::string> THEME_TAGS = {"Optimization", "AI", "Cloud"};
const std::vector<std::string> SECTOR_TAGS = {"Green", "Blue", "Orange"};
const std::vector<std::string> AUDIENCE_TAGS
    = {"Programmers", "Analysts", "Managers"};
const std::vector<std::string> CONTENT_TAGS
    = {"Timefold", "Constraints", "Metaheuristics", "Kubernetes"};

int speakerIndex(const ConferenceSchedule &c, const std::string &name)
{
    for (int k = 0; k < static_cast<int>(c.speakers.size()); ++k)
    {
        if (c.speakers[k].name == name)
            return k;
    }
    std::fprintf(stderr, "unknown speaker %s\n", name.c_str());
    std::abort();
}

ConferenceSchedule buildDemoConference()
{
    ConferenceSchedule c;
    c.name = "Conference";
    c.talkTypes = {"Lab", "Breakout"};

    auto slot = [](const char *id,
                   int startH,
                   int startM,
                   int endH,
                   int endM,
                   int talkType,
                   std::vector<std::string> tags)
    {
        Timeslot t;
        t.id = id;
        t.startMinute = startH * 60 + startM;
        t.endMinute = endH * 60 + endM;
        t.talkTypes = {talkType};
        t.tags = std::move(tags);
        return t;
    };
    c.timeslots = {
        slot("T1", 10, 15, 12, 15, LAB, {}),
        slot("T2", 10, 15, 11, 0, BREAKOUT, {}),
        slot("T3", 11, 30, 12, 15, BREAKOUT, {}),
        slot("T4", 13, 0, 15, 0, LAB, {"After lunch"}),
        slot("T5", 15, 30, 16, 15, BREAKOUT, {}),
        slot("T6", 16, 30, 17, 15, BREAKOUT, {}),
    };

    auto room = [](const char *id,
                   const char *name,
                   int capacity,
                   int talkType,
                   std::vector<std::string> tags)
    {
        Room r;
        r.id = id;
        r.name = name;
        r.capacity = capacity;
        r.talkTypes = {talkType};
        r.tags = std::move(tags);
        return r;
    };
    c.rooms = {
        room("R1", "Room A", 60, BREAKOUT, {"Recorded"}),
        room("R2", "Room B", 240, BREAKOUT, {}),
        room("R3", "Room C", 630, BREAKOUT, {"Recorded", "Large"}),
        room("R4", "Room D", 70, BREAKOUT, {"Recorded"}),
        room("R5", "Room E (LAB)", 490, LAB, {"Recorded"}),
    };

    const std::vector<std::string> speakerNames = {"Amy Cole",
                                                   "Beth Fox",
                                                   "Carl Green",
                                                   "Dan Jones",
                                                   "Elsa King",
                                                   "Flo Li",
                                                   "Gus Poe",
                                                   "Hugo Rye",
                                                   "Ivy Smith",
                                                   "Jay Watt",
                                                   "Amy Fox",
                                                   "Beth Green"};
    for (int k = 0; k < static_cast<int>(speakerNames.size()); ++k)
    {
        Speaker s;
        s.id = std::to_string(k + 1);
        s.name = speakerNames[k];
        c.speakers.push_back(std::move(s));
    }
    c.speakers.back().undesiredTimeslotTags = {"After lunch"};  // Beth Green

    JavaRandom random(0);
    auto addTalk = [&](const char *code,
                       const char *title,
                       int talkType,
                       std::vector<std::string> speakers,
                       int audienceLevel,
                       int favoriteCount,
                       int crowdControlRisk)
    {
        Talk t;
        t.code = code;
        t.title = title;
        t.talkType = talkType;
        for (const std::string &name : speakers)
            t.speakers.push_back(speakerIndex(c, name));
        t.themeTrackTags = {THEME_TAGS[random.nextInt(3)]};
        t.sectorTags = {SECTOR_TAGS[random.nextInt(3)]};
        t.audienceTypes = {AUDIENCE_TAGS[random.nextInt(3)]};
        t.audienceLevel = audienceLevel;
        t.contentTags = {CONTENT_TAGS[random.nextInt(4)]};
        t.language = "en";
        t.favoriteCount = favoriteCount;
        t.crowdControlRisk = crowdControlRisk;
        c.talks.push_back(std::move(t));
    };
    addTalk("S01", "Talk One", LAB, {"Amy Cole", "Beth Fox"}, 2, 551, 1);
    addTalk("S02", "Talk Two", LAB, {"Carl Green"}, 3, 528, 0);
    c.talks[0].undesiredRoomTags = {"Recorded"};  // S01
    addTalk("S03", "Talk Three", BREAKOUT, {"Dan Jones"}, 3, 497, 0);
    addTalk("S04", "Talk Four", BREAKOUT, {"Elsa King", "Flo Li"}, 1, 560, 0);
    addTalk("S05", "Talk Five", BREAKOUT, {"Gus Poe", "Hugo Rye"}, 1, 957, 0);
    addTalk("S06", "Talk Six", BREAKOUT, {"Ivy Smith"}, 1, 957, 0);
    c.talks[4].prerequisiteTalks = {1};  // S05 requires S02
    addTalk("S07", "Talk Seven", BREAKOUT, {"Jay Watt"}, 3, 568, 0);
    addTalk("S08", "Talk Eight", BREAKOUT, {"Amy Fox"}, 3, 183, 0);
    addTalk(
        "S09", "Talk Nine", BREAKOUT, {"Beth Green", "Amy Cole"}, 3, 619, 0);
    addTalk("S10", "Talk Ten", BREAKOUT, {"Beth Fox", "Carl Green"}, 3, 603, 1);
    addTalk(
        "S11", "Talk Eleven", BREAKOUT, {"Dan Jones", "Elsa King"}, 1, 39, 0);
    addTalk("S12", "Talk Twelve", BREAKOUT, {"Flo Li", "Gus Poe"}, 3, 977, 0);
    c.talks[10].mutuallyExclusiveTalksTags
        = {CONTENT_TAGS[random.nextInt(4)]};  // S11 (drawn after S12 in Java)
    addTalk("S13", "Talk Thirteen", BREAKOUT, {"Hugo Rye"}, 3, 494, 0);
    addTalk("S14", "Talk Fourteen", BREAKOUT, {"Ivy Smith"}, 3, 500, 0);
    addTalk("S15", "Talk Fifteen", BREAKOUT, {"Jay Watt"}, 2, 658, 0);
    c.talks[10].requiredRoomTags = {"Recorded"};  // S11

    return c;
}

// ---------------------------------------------------------------------------
// Scaled instance: the demo's shape (each slot and each room admits exactly
// one talk type, so type-compatible cells are scarce) at 2-4x the demo's
// size, with the full constraint surface (prereqs, mutex tags, crowd risk,
// speaker unavailability, required/preferred tags) deterministically drawn.
// Same instance for every solver seed: the generator seed is fixed.
// ---------------------------------------------------------------------------
ConferenceSchedule buildScaledConference(int talkCount)
{
    std::mt19937_64 rng(9001);
    ConferenceSchedule c;
    c.name = "Scaled-" + std::to_string(talkCount);
    c.talkTypes = {"Lab", "Breakout"};

    const std::vector<std::string> slotTags = {"After lunch", "Morning"};
    const std::vector<std::string> roomTags = {"Recorded", "Large"};
    const std::vector<std::string> languages = {"en", "de"};

    // Two days; per day 2 long LAB slots and 5 shorter BREAKOUT slots
    // (overlapping the labs, so room/speaker conflicts have teeth).
    for (int d = 0; d < 2; ++d)
    {
        const std::int64_t base = d * 1440;
        auto slot = [&](const char *suffix,
                        int startMin,
                        int endMin,
                        int talkType,
                        std::vector<std::string> tags)
        {
            Timeslot t;
            t.id = "D" + std::to_string(d + 1) + suffix;
            t.startMinute = base + startMin;
            t.endMinute = base + endMin;
            t.talkTypes = {talkType};
            t.tags = std::move(tags);
            c.timeslots.push_back(std::move(t));
        };
        slot("L1", 10 * 60 + 15, 12 * 60 + 15, LAB, {});
        slot("L2", 13 * 60, 15 * 60, LAB, {"After lunch"});
        slot("B1", 10 * 60 + 15, 11 * 60, BREAKOUT, {"Morning"});
        slot("B2", 11 * 60 + 30, 12 * 60 + 15, BREAKOUT, {"Morning"});
        slot("B3", 13 * 60, 13 * 60 + 45, BREAKOUT, {"After lunch"});
        slot("B4", 15 * 60 + 30, 16 * 60 + 15, BREAKOUT, {});
        slot("B5", 16 * 60 + 30, 17 * 60 + 15, BREAKOUT, {});
    }

    auto room = [&](std::string name,
                    int capacity,
                    int talkType,
                    std::vector<std::string> tags)
    {
        Room r;
        r.id = "R" + std::to_string(c.rooms.size() + 1);
        r.name = std::move(name);
        r.capacity = capacity;
        r.talkTypes = {talkType};
        r.tags = std::move(tags);
        c.rooms.push_back(std::move(r));
    };
    room("Lab X", 490, LAB, {"Recorded"});
    room("Lab Y", 350, LAB, {});
    room("Hall A", 60, BREAKOUT, {"Recorded"});
    room("Hall B", 240, BREAKOUT, {});
    room("Hall C", 630, BREAKOUT, {"Recorded", "Large"});
    room("Hall D", 70, BREAKOUT, {"Recorded"});
    room("Hall E", 120, BREAKOUT, {});
    room("Hall F", 300, BREAKOUT, {"Large"});
    room("Hall G", 90, BREAKOUT, {});
    // A couple of room outages (hard).
    c.rooms[3].unavailableTimeslots = {4};   // Hall B down in D1B3
    c.rooms[6].unavailableTimeslots = {11};  // Hall E down in D2B2

    const int speakerCount = 24;
    for (int k = 0; k < speakerCount; ++k)
    {
        Speaker s;
        s.id = std::to_string(k + 1);
        s.name = "Speaker " + std::to_string(k + 1);
        if (rng() % 4 == 0)
            s.unavailableTimeslots
                = {static_cast<int>(rng() % c.timeslots.size())};
        if (rng() % 4 == 0)
            s.undesiredTimeslotTags = {slotTags[rng() % slotTags.size()]};
        if (rng() % 5 == 0)
            s.preferredRoomTags = {roomTags[rng() % roomTags.size()]};
        c.speakers.push_back(std::move(s));
    }

    const int labTalks = std::max(1, talkCount / 7);
    for (int i = 0; i < talkCount; ++i)
    {
        Talk t;
        t.code = "S" + std::to_string(i + 1);
        t.title = "Talk " + std::to_string(i + 1);
        t.talkType = i < labTalks ? LAB : BREAKOUT;
        t.speakers = {static_cast<int>(rng() % speakerCount)};
        if (rng() % 3 == 0)
        {
            const int other = static_cast<int>(rng() % speakerCount);
            if (other != t.speakers[0])
                t.speakers.push_back(other);
        }
        t.themeTrackTags = {THEME_TAGS[rng() % THEME_TAGS.size()]};
        t.sectorTags = {SECTOR_TAGS[rng() % SECTOR_TAGS.size()]};
        t.audienceTypes = {AUDIENCE_TAGS[rng() % AUDIENCE_TAGS.size()]};
        t.audienceLevel = 1 + static_cast<int>(rng() % 3);
        t.contentTags = {CONTENT_TAGS[rng() % CONTENT_TAGS.size()]};
        t.language = languages[rng() % languages.size()];
        t.favoriteCount = static_cast<int>(rng() % 1000);
        t.crowdControlRisk = (rng() % 5 == 0) ? 1 : 0;
        if (rng() % 5 == 0)
            t.mutuallyExclusiveTalksTags
                = {CONTENT_TAGS[rng() % CONTENT_TAGS.size()]};
        if (i > labTalks && rng() % 5 == 0)
            t.prerequisiteTalks = {static_cast<int>(rng() % i)};
        if (rng() % 6 == 0)
            t.undesiredRoomTags = {roomTags[rng() % roomTags.size()]};
        if (rng() % 6 == 0)
            t.preferredTimeslotTags = {slotTags[rng() % slotTags.size()]};
        if (rng() % 8 == 0)
            t.requiredRoomTags = {"Recorded"};
        c.talks.push_back(std::move(t));
    }
    return c;
}

void printBreakdown(const Breakdown &b)
{
    struct Row
    {
        const char *name;
        std::int64_t units;
    };
    const Row rows[] = {
        {"H.talkTypeCompatibility", b.talkTypeCompatibility},
        {"H.roomUnavailableTimeslot", b.roomUnavailableTimeslot},
        {"H.roomConflict", b.roomConflict},
        {"H.speakerUnavailableTimeslot", b.speakerUnavailableTimeslot},
        {"H.speakerConflict", b.speakerConflict},
        {"H.talkPrerequisiteTalks", b.talkPrerequisiteTalks},
        {"H.talkMutuallyExclusiveTalksTags", b.talkMutuallyExclusiveTalksTags},
        {"H.consecutiveTalksPause", b.consecutiveTalksPause},
        {"H.crowdControl", b.crowdControl},
        {"H.speakerRequiredTimeslotTags", b.speakerRequiredTimeslotTags},
        {"H.speakerProhibitedTimeslotTags", b.speakerProhibitedTimeslotTags},
        {"H.talkRequiredTimeslotTags", b.talkRequiredTimeslotTags},
        {"H.talkProhibitedTimeslotTags", b.talkProhibitedTimeslotTags},
        {"H.speakerRequiredRoomTags", b.speakerRequiredRoomTags},
        {"H.speakerProhibitedRoomTags", b.speakerProhibitedRoomTags},
        {"H.talkRequiredRoomTags", b.talkRequiredRoomTags},
        {"H.talkProhibitedRoomTags", b.talkProhibitedRoomTags},
        {"S.themeTrackConflict", b.themeTrackConflict},
        {"S.themeTrackRoomStability", b.themeTrackRoomStability},
        {"S.sectorConflict", b.sectorConflict},
        {"S.audienceTypeThemeTrackConflict", b.audienceTypeThemeTrackConflict},
        {"S.contentAudienceLevelFlowViolation",
         b.contentAudienceLevelFlowViolation},
        {"S.contentConflict", b.contentConflict},
        {"S.sameDayTalks", b.sameDayTalks},
        {"S.popularTalks", b.popularTalks},
        {"S.speakerPreferredTimeslotTags", b.speakerPreferredTimeslotTags},
        {"S.speakerUndesiredTimeslotTags", b.speakerUndesiredTimeslotTags},
        {"S.talkPreferredTimeslotTags", b.talkPreferredTimeslotTags},
        {"S.talkUndesiredTimeslotTags", b.talkUndesiredTimeslotTags},
        {"S.speakerPreferredRoomTags", b.speakerPreferredRoomTags},
        {"S.speakerUndesiredRoomTags", b.speakerUndesiredRoomTags},
        {"S.talkPreferredRoomTags", b.talkPreferredRoomTags},
        {"S.talkUndesiredRoomTags", b.talkUndesiredRoomTags},
        {"S.speakerMakespan", b.speakerMakespan},
        {"R.audienceTypeDiversity", b.audienceTypeDiversity},
        {"R.audienceLevelDiversity", b.audienceLevelDiversity},
        {"R.languageDiversity", b.languageDiversity},
    };
    for (const Row &row : rows)
    {
        if (row.units != 0)
            std::printf("CS_BENCH breakdown %s units=%lld\n",
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
                     "usage: cs_quality_bench <demo|scaled:<talks>> <seed> "
                     "<seconds> [key=value ...]\n");
        return 2;
    }
    const std::string instanceArg = argv[1];
    const std::uint64_t seed = std::strtoull(argv[2], nullptr, 10);
    const int seconds = std::atoi(argv[3]);

    ConferenceSchedule conference;
    if (instanceArg == "demo")
    {
        conference = buildDemoConference();
    }
    else if (instanceArg.rfind("scaled:", 0) == 0)
    {
        conference = buildScaledConference(std::atoi(instanceArg.c_str() + 7));
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
        if (key == "compatProb")
            config.compatibleValueProbability = value;
        else if (key == "la")
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
        else if (key == "segments")
            config.restartSegments = static_cast<int>(value);
        else if (key == "asMove")
            config.ruinRecreateAsMoveWeight = value;
        else if (key == "asMoveMaxRuin")
            config.ruinRecreateAsMoveMaxRuin = static_cast<int>(value);
        else if (key == "descent")
            config.descentWeight = value;
        else if (key == "hot")
            config.hotTalkProbability = value;
        else
        {
            std::fprintf(stderr, "unknown knob %s\n", key.c_str());
            return 2;
        }
    }

    std::printf("CS_BENCH instance name=%s talks=%zu timeslots=%zu rooms=%zu "
                "speakers=%zu seed=%llu budget=%ds\n",
                conference.name.c_str(),
                conference.talks.size(),
                conference.timeslots.size(),
                conference.rooms.size(),
                conference.speakers.size(),
                static_cast<unsigned long long>(seed),
                seconds);

    const auto t0 = std::chrono::steady_clock::now();
    // Sustained move speed, estimated from the improvement trajectory (each
    // engine step evaluates one move) — the st_quality_bench convention.
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
        std::printf("CS_BENCH best t=%lldms step=%lld score=%s\n",
                    static_cast<long long>(ms),
                    static_cast<long long>(step),
                    sc.toString().c_str());
    };

    Solver solver(config);
    solver.solve(conference);

    const double movesPerSec = lastMs > 0
                                   ? 1000.0 * static_cast<double>(lastStep)
                                         / static_cast<double>(lastMs)
                                   : 0.0;
    Breakdown breakdown = ScoreCalculator::computeBreakdown(conference);
    printBreakdown(breakdown);
    std::printf("CS_BENCH_FINAL hard=%lld soft=%.0f feasible=%s "
                "first_feasible_ms=%lld moves_per_sec=%.0f\n",
                static_cast<long long>(conference.score.hard),
                conference.score.soft,
                conference.score.isFeasible() ? "true" : "false",
                feasibleMs,
                movesPerSec);
    return 0;
}

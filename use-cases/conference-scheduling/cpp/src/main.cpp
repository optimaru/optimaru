// Demo: reproduces the Java quickstart's DemoDataGenerator (including its
// java.util.Random(0) tag draws), solves for N seconds and asserts the result
// is feasible (hard == 0).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cs/domain.hpp"
#include "cs/score_calculator.hpp"
#include "cs/solver.hpp"

using namespace cs;

namespace
{

// ---------------------------------------------------------------------------
// java.util.Random (48-bit LCG), enough for nextInt(bound) — the demo tags
// must match the Java generator draw for draw.
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
            // Reject draws that would bias the modulo (int overflow test).
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

    // Six timeslots on one day (the Java generator uses "today"; minutes
    // from that day's midnight).
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
    // Creation (and therefore random-draw) order matches the Java generator.
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

void printSchedule(const ConferenceSchedule &c)
{
    std::printf("  %-4s %-9s %-4s %-13s %s\n",
                "talk",
                "type",
                "slot",
                "room",
                "speakers");
    for (const Talk &talk : c.talks)
    {
        std::string speakers;
        for (int k : talk.speakers)
        {
            if (!speakers.empty())
                speakers += ", ";
            speakers += c.speakers[k].name;
        }
        if (talk.slotRoom < 0)
        {
            std::printf("  %-4s %-9s (unassigned)  %s\n",
                        talk.code.c_str(),
                        c.talkTypes[talk.talkType].c_str(),
                        speakers.c_str());
            continue;
        }
        const int s = c.timeslotIndexOf(talk.slotRoom);
        const int r = c.roomIndexOf(talk.slotRoom);
        std::printf("  %-4s %-9s %-4s %-13s %s\n",
                    talk.code.c_str(),
                    c.talkTypes[talk.talkType].c_str(),
                    c.timeslots[s].id.c_str(),
                    c.rooms[r].name.c_str(),
                    speakers.c_str());
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char **argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;

    ConferenceSchedule conference = buildDemoConference();
    std::printf("== Conference scheduling demo (talks %zu, timeslots %zu, "
                "rooms %zu, speakers %zu, %ds) ==\n",
                conference.talks.size(),
                conference.timeslots.size(),
                conference.rooms.size(),
                conference.speakers.size(),
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
    solver.solve(conference);

    std::printf("\n  final score: %s  (feasible=%s)\n\n",
                conference.score.toString().c_str(),
                conference.score.isFeasible() ? "true" : "false");
    printSchedule(conference);

    bool allAssigned = true;
    for (const Talk &talk : conference.talks)
    {
        if (talk.slotRoom < 0)
            allAssigned = false;
    }
    std::printf("  all talks assigned: %s\n", allAssigned ? "OK" : "FAIL");

    if (conference.score.hard == 0 && allAssigned)
    {
        std::printf("  hard 0 reached: OK\n");
        return 0;
    }
    std::printf("  hard 0 NOT reached (hard=%lld)\n",
                static_cast<long long>(conference.score.hard));
    return 1;
}

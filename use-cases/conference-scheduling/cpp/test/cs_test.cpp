// Conference-scheduling unit tests (engine test/ style: check() + non-zero exit
// on any failure):
//   (a) every constraint verified in isolation against hand-computed
//       breakdown units and spot-checked total scores
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's ScoreDirector protocol (before/set/after)
//   (c) a random instance solved under EnvironmentMode::FULL_ASSERT
//   (d) pinned talks keep their assignment through a solve; an easy instance
//       reaches hard 0
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "cs/domain.hpp"
#include "cs/incremental_calculator.hpp"
#include "cs/score_calculator.hpp"
#include "cs/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace cs;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

void checkUnits(const std::string &label,
                std::int64_t actual,
                std::int64_t expected)
{
    const bool ok = actual == expected;
    std::printf("  [%s] %-46s got=%lld expected=%lld\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                static_cast<long long>(actual),
                static_cast<long long>(expected));
    if (!ok)
        ++failures;
}

void checkScore(const std::string &label,
                const Score &actual,
                long long expectedHard,
                double expectedSoft)
{
    const bool ok = (actual.hard == expectedHard
                     && std::fabs(actual.soft - expectedSoft) < 1e-9);
    std::printf("  [%s] %-46s got=%s expected=%lldhard/%gsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedSoft);
    if (!ok)
        ++failures;
}

// -------------------------------------------------------------------------
// Base problem: one talk type "B" (0), five timeslots and three rooms, no
// tags/unavailability. Slot map (minutes; day = minute / 1440):
//   s0 540-600 (day 0)          s1 540-600 (day 0, distinct slot, same time)
//   s2 570-630 (overlaps s0/s1 by 30)
//   s3 630-690 (touches s2, 30 min after s0)
//   s4 1980-2040 (day 1)        s5 3420-3480 (day 2)
// -------------------------------------------------------------------------
ConferenceSchedule makeBase()
{
    ConferenceSchedule c;
    c.name = "test";
    c.talkTypes = {"B"};
    auto slot = [](const char *id, std::int64_t start, std::int64_t end)
    {
        Timeslot t;
        t.id = id;
        t.startMinute = start;
        t.endMinute = end;
        t.talkTypes = {0};
        return t;
    };
    c.timeslots = {slot("s0", 540, 600),
                   slot("s1", 540, 600),
                   slot("s2", 570, 630),
                   slot("s3", 630, 690),
                   slot("s4", 1980, 2040),
                   slot("s5", 3420, 3480)};
    auto room = [](const char *id, int capacity)
    {
        Room r;
        r.id = id;
        r.capacity = capacity;
        r.talkTypes = {0};
        return r;
    };
    c.rooms = {room("r0", 100), room("r1", 200), room("r2", 300)};
    for (int k = 0; k < 4; ++k)
    {
        Speaker s;
        s.id = "k" + std::to_string(k);
        s.name = s.id;
        c.speakers.push_back(std::move(s));
    }
    return c;
}

// Adds a talk assigned to (slot, room); -1 slot leaves it unassigned.
int addTalk(ConferenceSchedule &c,
            const char *code,
            std::vector<int> speakers,
            int slot = -1,
            int room = 0)
{
    Talk t;
    t.code = code;
    t.talkType = 0;
    t.speakers = std::move(speakers);
    t.language = "en";
    t.slotRoom = slot < 0 ? -1 : c.slotRoomOf(slot, room);
    c.talks.push_back(std::move(t));
    return static_cast<int>(c.talks.size()) - 1;
}

// -------------------------------------------------------------------------
// (a) Each constraint in isolation: breakdown units vs hand-computed values.
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Verifying each constraint in isolation ==\n");

    // h0: talkTypeCompatibility (synthetic value-range replacement). A "L"
    // talk in a "B"-only slot and room violates both halves: 2 x 60.
    {
        ConferenceSchedule c = makeBase();
        c.talkTypes.push_back("L");
        int i = addTalk(c, "A", {0}, 0, 0);
        c.talks[i].talkType = 1;
        Breakdown b = ScoreCalculator::computeBreakdown(c);
        checkUnits("talkTypeCompatibility both", b.talkTypeCompatibility, 120);
        checkScore("talkTypeCompatibility score", b.toScore(), -12000000, 0);
    }
    {
        ConferenceSchedule c = makeBase();
        c.talkTypes.push_back("L");
        c.rooms[0].talkTypes = {0, 1};  // room now admits "L"
        int i = addTalk(c, "A", {0}, 0, 0);
        c.talks[i].talkType = 1;
        checkUnits("talkTypeCompatibility slot only",
                   ScoreCalculator::computeBreakdown(c).talkTypeCompatibility,
                   60);
    }
    // h1: roomUnavailableTimeslot — duration.
    {
        ConferenceSchedule c = makeBase();
        c.rooms[0].unavailableTimeslots = {0};
        addTalk(c, "A", {0}, 0, 0);
        Breakdown b = ScoreCalculator::computeBreakdown(c);
        checkUnits(
            "roomUnavailableTimeslot hit", b.roomUnavailableTimeslot, 60);
        checkScore("roomUnavailableTimeslot score", b.toScore(), -6000000, 0);
    }
    {
        ConferenceSchedule c = makeBase();
        c.rooms[0].unavailableTimeslots = {0};
        addTalk(c, "A", {0}, 3, 0);
        checkUnits("roomUnavailableTimeslot miss",
                   ScoreCalculator::computeBreakdown(c).roomUnavailableTimeslot,
                   0);
    }
    // h2: roomConflict — overlap minutes; distinct slots with equal times.
    {
        ConferenceSchedule c = makeBase();
        addTalk(c, "A", {0}, 0, 0);
        addTalk(c, "B", {1}, 1, 0);
        checkUnits("roomConflict full overlap",
                   ScoreCalculator::computeBreakdown(c).roomConflict,
                   60);
    }
    {
        ConferenceSchedule c = makeBase();
        addTalk(c, "A", {0}, 0, 0);
        addTalk(c, "B", {1}, 2, 0);  // s0/s2 overlap 570-600
        checkUnits("roomConflict partial overlap",
                   ScoreCalculator::computeBreakdown(c).roomConflict,
                   30);
    }
    {
        ConferenceSchedule c = makeBase();
        addTalk(c, "A", {0}, 0, 0);
        addTalk(c, "B", {1}, 3, 0);  // disjoint times, same room
        checkUnits("roomConflict disjoint",
                   ScoreCalculator::computeBreakdown(c).roomConflict,
                   0);
    }
    // h3: speakerUnavailableTimeslot — duration per unavailable speaker.
    {
        ConferenceSchedule c = makeBase();
        c.speakers[0].unavailableTimeslots = {0};
        c.speakers[1].unavailableTimeslots = {0};
        addTalk(c, "A", {0, 1}, 0, 0);
        checkUnits(
            "speakerUnavailableTimeslot two speakers",
            ScoreCalculator::computeBreakdown(c).speakerUnavailableTimeslot,
            120);
    }
    // h4: speakerConflict — overlap per shared speaker; the overlapping pair
    // also lacks the consecutive pause (h7).
    {
        ConferenceSchedule c = makeBase();
        addTalk(c, "A", {0, 1}, 0, 0);
        addTalk(c, "B", {0, 1}, 1, 1);
        Breakdown b = ScoreCalculator::computeBreakdown(c);
        checkUnits("speakerConflict two shared", b.speakerConflict, 120);
        checkUnits(
            "consecutiveTalksPause via overlap", b.consecutiveTalksPause, 120);
        checkScore("speakerConflict score", b.toScore(), -(10 * 120 + 120), 0);
    }
    // h5: talkPrerequisiteTalks — combined duration when the prerequisite
    // does not end before the dependent starts.
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 3, 0);  // prerequisite at s3 (630-690)
        addTalk(c, "B", {1}, 0, 1);          // dependent at s0 (540-600)
        c.talks[1].prerequisiteTalks = {a};
        checkUnits("talkPrerequisiteTalks violated",
                   ScoreCalculator::computeBreakdown(c).talkPrerequisiteTalks,
                   120);
    }
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 0, 0);  // ends 600
        addTalk(c, "B", {1}, 3, 1);          // starts 630
        c.talks[1].prerequisiteTalks = {a};
        checkUnits("talkPrerequisiteTalks satisfied",
                   ScoreCalculator::computeBreakdown(c).talkPrerequisiteTalks,
                   0);
    }
    // h6: talkMutuallyExclusiveTalksTags — shared tags x overlap.
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 0, 0);
        int b = addTalk(c, "B", {1}, 1, 1);
        c.talks[a].mutuallyExclusiveTalksTags = {"M", "N"};
        c.talks[b].mutuallyExclusiveTalksTags = {"M", "N"};
        checkUnits(
            "mutuallyExclusiveTalksTags",
            ScoreCalculator::computeBreakdown(c).talkMutuallyExclusiveTalksTags,
            120);
    }
    // h7: consecutiveTalksPause — shared speaker, same day, gap < minimum.
    {
        ConferenceSchedule c = makeBase();
        addTalk(c, "A", {0}, 2, 0);  // 570-630
        addTalk(c, "B", {0}, 3, 1);  // 630-690: gap 0
        checkUnits("consecutiveTalksPause gap 0",
                   ScoreCalculator::computeBreakdown(c).consecutiveTalksPause,
                   120);
    }
    {
        ConferenceSchedule c = makeBase();
        addTalk(c, "A", {0}, 0, 0);  // ends 600
        addTalk(c, "B", {0}, 3, 1);  // starts 630: gap 30 == minimum
        checkUnits("consecutiveTalksPause gap 30 ok",
                   ScoreCalculator::computeBreakdown(c).consecutiveTalksPause,
                   0);
    }
    {
        ConferenceSchedule c = makeBase();
        c.minimumConsecutiveTalksPauseInMinutes = 31;
        addTalk(c, "A", {0}, 0, 0);
        addTalk(c, "B", {0}, 3, 1);  // gap 30 < 31
        checkUnits("consecutiveTalksPause configurable minimum",
                   ScoreCalculator::computeBreakdown(c).consecutiveTalksPause,
                   120);
    }
    {
        ConferenceSchedule c = makeBase();
        addTalk(c, "A", {0}, 0, 0);  // day 0
        addTalk(c, "B", {0}, 4, 1);  // day 1: pause exists across days
        checkUnits("consecutiveTalksPause different days",
                   ScoreCalculator::computeBreakdown(c).consecutiveTalksPause,
                   0);
    }
    // h8: crowdControl — a risk>0 talk with >= 2 other risk>0 talks in its
    // timeslot; exactly one other is the wanted pairing (no penalty).
    {
        ConferenceSchedule c = makeBase();
        for (int r = 0; r < 3; ++r)
        {
            int i = addTalk(c, "T", {r}, 0, r);
            c.talks[i].crowdControlRisk = 1;
        }
        checkUnits("crowdControl three risk talks",
                   ScoreCalculator::computeBreakdown(c).crowdControl,
                   180);
    }
    {
        ConferenceSchedule c = makeBase();
        for (int r = 0; r < 2; ++r)
        {
            int i = addTalk(c, "T", {r}, 0, r);
            c.talks[i].crowdControlRisk = 1;
        }
        checkUnits("crowdControl paired ok",
                   ScoreCalculator::computeBreakdown(c).crowdControl,
                   0);
    }
    {
        ConferenceSchedule c = makeBase();
        int i = addTalk(c, "T", {0}, 0, 0);
        c.talks[i].crowdControlRisk = 1;
        checkUnits("crowdControl alone ok",
                   ScoreCalculator::computeBreakdown(c).crowdControl,
                   0);
    }
    // h9-h16 + s12-s19: the sixteen tag constraints. Slot s0 carries {TS},
    // room r0 carries {RM}.
    {
        ConferenceSchedule c = makeBase();
        c.timeslots[0].tags = {"TS"};
        c.rooms[0].tags = {"RM"};
        int i = addTalk(c, "A", {0, 1}, 0, 0);
        Talk &t = c.talks[i];
        t.requiredTimeslotTags = {"TS", "X"};  // missing X
        t.prohibitedTimeslotTags = {"TS"};     // prevailing TS
        t.preferredTimeslotTags = {"Y"};       // missing Y
        t.undesiredTimeslotTags = {"TS"};      // prevailing TS
        t.requiredRoomTags = {"RM", "Z"};      // missing Z
        t.prohibitedRoomTags = {"RM"};         // prevailing RM
        t.preferredRoomTags = {"W"};           // missing W
        t.undesiredRoomTags = {"RM"};          // prevailing RM
        Speaker &k0 = c.speakers[0];
        k0.requiredTimeslotTags = {"X"};         // missing
        k0.prohibitedTimeslotTags = {"TS"};      // prevailing
        k0.preferredTimeslotTags = {"TS", "Y"};  // missing Y only
        k0.undesiredTimeslotTags = {"TS"};       // prevailing
        Speaker &k1 = c.speakers[1];
        k1.requiredRoomTags = {"Z", "W"};  // missing both
        k1.prohibitedRoomTags = {"RM"};    // prevailing
        k1.preferredRoomTags = {"W"};      // missing
        k1.undesiredRoomTags = {"RM"};     // prevailing
        Breakdown b = ScoreCalculator::computeBreakdown(c);
        checkUnits("talkRequiredTimeslotTags", b.talkRequiredTimeslotTags, 60);
        checkUnits(
            "talkProhibitedTimeslotTags", b.talkProhibitedTimeslotTags, 60);
        checkUnits(
            "talkPreferredTimeslotTags", b.talkPreferredTimeslotTags, 60);
        checkUnits(
            "talkUndesiredTimeslotTags", b.talkUndesiredTimeslotTags, 60);
        checkUnits("talkRequiredRoomTags", b.talkRequiredRoomTags, 60);
        checkUnits("talkProhibitedRoomTags", b.talkProhibitedRoomTags, 60);
        checkUnits("talkPreferredRoomTags", b.talkPreferredRoomTags, 60);
        checkUnits("talkUndesiredRoomTags", b.talkUndesiredRoomTags, 60);
        checkUnits(
            "speakerRequiredTimeslotTags", b.speakerRequiredTimeslotTags, 60);
        checkUnits("speakerProhibitedTimeslotTags",
                   b.speakerProhibitedTimeslotTags,
                   60);
        checkUnits(
            "speakerPreferredTimeslotTags", b.speakerPreferredTimeslotTags, 60);
        checkUnits(
            "speakerUndesiredTimeslotTags", b.speakerUndesiredTimeslotTags, 60);
        checkUnits(
            "speakerRequiredRoomTags (2 tags)", b.speakerRequiredRoomTags, 120);
        checkUnits(
            "speakerProhibitedRoomTags", b.speakerProhibitedRoomTags, 60);
        checkUnits("speakerPreferredRoomTags", b.speakerPreferredRoomTags, 60);
        checkUnits("speakerUndesiredRoomTags", b.speakerUndesiredRoomTags, 60);
    }
    // Required tags with an empty tag set on the slot: all are missing.
    {
        ConferenceSchedule c = makeBase();
        int i = addTalk(c, "A", {}, 0, 0);
        c.talks[i].requiredTimeslotTags = {"X", "Y", "Z"};
        checkUnits(
            "talkRequiredTimeslotTags all missing",
            ScoreCalculator::computeBreakdown(c).talkRequiredTimeslotTags,
            180);
    }
    // s1 + s2 + s5: theme conflict, room stability and the audience-theme
    // combination on one overlapping same-day pair in different rooms.
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 0, 0);
        int b = addTalk(c, "B", {1}, 1, 1);
        c.talks[a].themeTrackTags = {"AI"};
        c.talks[b].themeTrackTags = {"AI"};
        c.talks[a].audienceTypes = {"Dev"};
        c.talks[b].audienceTypes = {"Dev"};
        Breakdown bd = ScoreCalculator::computeBreakdown(c);
        checkUnits("themeTrackConflict", bd.themeTrackConflict, 60);
        checkUnits("themeTrackRoomStability", bd.themeTrackRoomStability, 120);
        checkUnits("audienceTypeThemeTrackConflict",
                   bd.audienceTypeThemeTrackConflict,
                   60);
        checkUnits("audienceTypeDiversity (different slots)",
                   bd.audienceTypeDiversity,
                   0);
    }
    // s2: same-day shared-theme pair in the SAME room is stable.
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 0, 0);
        int b = addTalk(c, "B", {1}, 3, 0);  // disjoint times, same room
        c.talks[a].themeTrackTags = {"AI"};
        c.talks[b].themeTrackTags = {"AI"};
        checkUnits("themeTrackRoomStability same room",
                   ScoreCalculator::computeBreakdown(c).themeTrackRoomStability,
                   0);
    }
    // s3: sectorConflict.
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 0, 0);
        int b = addTalk(c, "B", {1}, 2, 1);  // overlap 30
        c.talks[a].sectorTags = {"Fin", "Gov"};
        c.talks[b].sectorTags = {"Fin", "Gov"};
        checkUnits("sectorConflict two sectors",
                   ScoreCalculator::computeBreakdown(c).sectorConflict,
                   60);
    }
    // s4 + s6 + s9: same-timeslot diversity rewards.
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 0, 0);
        int b = addTalk(c, "B", {1}, 0, 1);  // same slot, different rooms
        c.talks[a].audienceTypes = {"Dev"};
        c.talks[b].audienceTypes = {"Dev"};
        c.talks[a].audienceLevel = 1;
        c.talks[b].audienceLevel = 2;
        c.talks[b].language = "de";
        Breakdown bd = ScoreCalculator::computeBreakdown(c);
        checkUnits("audienceTypeDiversity", bd.audienceTypeDiversity, 60);
        checkUnits("audienceLevelDiversity", bd.audienceLevelDiversity, 60);
        checkUnits("languageDiversity", bd.languageDiversity, 60);
        // rewards: +1*60 + 1*60 + 10*60 = +720 soft.
        checkScore("diversity rewards score", bd.toScore(), 0, 720);
    }
    // s7: contentAudienceLevelFlowViolation — lower level ends after the
    // higher level starts.
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 3, 0);  // level 1 at 630-690
        int b = addTalk(c, "B", {1}, 0, 1);  // level 2 at 540-600
        c.talks[a].audienceLevel = 1;
        c.talks[b].audienceLevel = 2;
        c.talks[a].contentTags = {"CT"};
        c.talks[b].contentTags = {"CT"};
        checkUnits("contentAudienceLevelFlowViolation",
                   ScoreCalculator::computeBreakdown(c)
                       .contentAudienceLevelFlowViolation,
                   120);
    }
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 0, 0);  // level 1 ends 600
        int b = addTalk(c, "B", {1}, 3, 1);  // level 2 starts 630
        c.talks[a].audienceLevel = 1;
        c.talks[b].audienceLevel = 2;
        c.talks[a].contentTags = {"CT"};
        c.talks[b].contentTags = {"CT"};
        checkUnits("flow satisfied (lower level first)",
                   ScoreCalculator::computeBreakdown(c)
                       .contentAudienceLevelFlowViolation,
                   0);
    }
    // s8: contentConflict (equal levels so the flow constraint stays quiet).
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 0, 0);
        int b = addTalk(c, "B", {1}, 1, 1);
        c.talks[a].contentTags = {"CT"};
        c.talks[b].contentTags = {"CT"};
        Breakdown bd = ScoreCalculator::computeBreakdown(c);
        checkUnits("contentConflict", bd.contentConflict, 60);
        checkUnits("flow quiet on equal levels",
                   bd.contentAudienceLevelFlowViolation,
                   0);
    }
    // s10: sameDayTalks — shared content/theme on different days.
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 0, 0);  // day 0
        int b = addTalk(c, "B", {1}, 4, 1);  // day 1
        c.talks[a].contentTags = {"CT"};
        c.talks[b].contentTags = {"CT"};
        c.talks[a].themeTrackTags = {"AI"};
        c.talks[b].themeTrackTags = {"AI"};
        checkUnits("sameDayTalks theme+content",
                   ScoreCalculator::computeBreakdown(c).sameDayTalks,
                   240);
    }
    // s11: popularTalks — the more favorited talk sits in the smaller room.
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 0, 2);  // fav 10, capacity 300
        int b = addTalk(c, "B", {1}, 3, 0);  // fav 20, capacity 100
        c.talks[a].favoriteCount = 10;
        c.talks[b].favoriteCount = 20;
        checkUnits("popularTalks violated",
                   ScoreCalculator::computeBreakdown(c).popularTalks,
                   120);
    }
    {
        ConferenceSchedule c = makeBase();
        int a = addTalk(c, "A", {0}, 0, 0);  // fav 10, capacity 100
        int b = addTalk(c, "B", {1}, 3, 2);  // fav 20, capacity 300
        c.talks[a].favoriteCount = 10;
        c.talks[b].favoriteCount = 20;
        checkUnits("popularTalks satisfied",
                   ScoreCalculator::computeBreakdown(c).popularTalks,
                   0);
    }
    // s20: speakerMakespan — talks spanning 3 calendar days (span 2) cost
    // one 8-hour day.
    {
        ConferenceSchedule c = makeBase();
        addTalk(c, "A", {0}, 0, 0);  // day 0
        addTalk(c, "B", {0}, 5, 1);  // day 2
        checkUnits("speakerMakespan span 2",
                   ScoreCalculator::computeBreakdown(c).speakerMakespan,
                   480);
    }
    {
        ConferenceSchedule c = makeBase();
        addTalk(c, "A", {0}, 0, 0);  // day 0
        addTalk(c, "B", {0}, 4, 1);  // day 1: span 1 is fine
        checkUnits("speakerMakespan span 1 ok",
                   ScoreCalculator::computeBreakdown(c).speakerMakespan,
                   0);
    }
    // Unassigned talks contribute nothing.
    {
        ConferenceSchedule c = makeBase();
        c.speakers[0].unavailableTimeslots = {0};
        addTalk(c, "A", {0}, 0, 0);
        addTalk(c, "B", {0});  // unassigned, would conflict if placed
        checkScore("unassigned contributes nothing",
                   ScoreCalculator::calculateScore(c),
                   -6000,
                   0);
    }
    // Pinned talks are scored like any other talk.
    {
        ConferenceSchedule c = makeBase();
        addTalk(c, "A", {0}, 0, 0);
        c.talks.back().pinned = true;
        addTalk(c, "B", {1}, 1, 0);
        checkScore("pinned talk still scored",
                   ScoreCalculator::calculateScore(c),
                   -60000,
                   0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random mid-size instance touching every constraint, for the incremental
// cross-check and the solves.
// -------------------------------------------------------------------------
ConferenceSchedule buildRandomInstance(int talkCount, std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    ConferenceSchedule c;
    c.name = "random";
    c.talkTypes = {"B", "L"};
    const std::vector<std::string> themes = {"AI", "Cloud", "Opt"};
    const std::vector<std::string> sectors = {"Fin", "Gov"};
    const std::vector<std::string> audiences = {"Dev", "Ops", "Mgr"};
    const std::vector<std::string> contents = {"C1", "C2", "C3", "C4"};
    const std::vector<std::string> slotTags = {"Morning", "After lunch"};
    const std::vector<std::string> roomTags = {"Recorded", "Large"};
    const std::vector<std::string> languages = {"en", "de"};

    // Eight slots over two days; two are "L"-typed, some tagged.
    for (int d = 0; d < 2; ++d)
    {
        for (int p = 0; p < 4; ++p)
        {
            Timeslot t;
            t.id = "s" + std::to_string(d * 4 + p);
            // 540 + 50p keeps 10-minute gaps between 40-minute slots so the
            // pause constraint has near misses.
            t.startMinute = d * 1440 + 540 + 50 * p;
            t.endMinute = t.startMinute + 40;
            t.talkTypes
                = (p == 3) ? std::vector<int>{1} : std::vector<int>{0, 1};
            if (rng() % 3 == 0)
                t.tags = {slotTags[rng() % slotTags.size()]};
            c.timeslots.push_back(std::move(t));
        }
    }
    for (int r = 0; r < 4; ++r)
    {
        Room room;
        room.id = "r" + std::to_string(r);
        room.capacity = 100 + 100 * static_cast<int>(rng() % 4);
        room.talkTypes
            = (r == 3) ? std::vector<int>{1} : std::vector<int>{0, 1};
        if (rng() % 2 == 0)
            room.tags = {roomTags[rng() % roomTags.size()]};
        if (rng() % 3 == 0)
            room.unavailableTimeslots
                = {static_cast<int>(rng() % c.timeslots.size())};
        c.rooms.push_back(std::move(room));
    }
    for (int k = 0; k < 8; ++k)
    {
        Speaker s;
        s.id = "k" + std::to_string(k);
        s.name = s.id;
        if (rng() % 3 == 0)
            s.unavailableTimeslots
                = {static_cast<int>(rng() % c.timeslots.size())};
        if (rng() % 3 == 0)
            s.undesiredTimeslotTags = {slotTags[rng() % slotTags.size()]};
        if (rng() % 4 == 0)
            s.requiredRoomTags = {roomTags[rng() % roomTags.size()]};
        if (rng() % 4 == 0)
            s.preferredTimeslotTags = {slotTags[rng() % slotTags.size()]};
        if (rng() % 5 == 0)
            s.prohibitedTimeslotTags = {slotTags[rng() % slotTags.size()]};
        c.speakers.push_back(std::move(s));
    }
    for (int i = 0; i < talkCount; ++i)
    {
        Talk t;
        t.code = "T" + std::to_string(i);
        t.talkType = static_cast<int>(rng() % 2);
        t.speakers = {static_cast<int>(rng() % c.speakers.size())};
        if (rng() % 3 == 0)
        {
            int other = static_cast<int>(rng() % c.speakers.size());
            if (other != t.speakers[0])
                t.speakers.push_back(other);
        }
        t.themeTrackTags = {themes[rng() % themes.size()]};
        t.sectorTags = {sectors[rng() % sectors.size()]};
        t.audienceTypes = {audiences[rng() % audiences.size()]};
        t.audienceLevel = 1 + static_cast<int>(rng() % 3);
        t.contentTags = {contents[rng() % contents.size()]};
        t.language = languages[rng() % languages.size()];
        t.favoriteCount = static_cast<int>(rng() % 1000);
        t.crowdControlRisk = (rng() % 4 == 0) ? 1 : 0;
        if (rng() % 4 == 0)
            t.mutuallyExclusiveTalksTags = {contents[rng() % contents.size()]};
        if (i > 0 && rng() % 4 == 0)
            t.prerequisiteTalks = {static_cast<int>(rng() % i)};
        if (rng() % 4 == 0)
            t.requiredTimeslotTags = {slotTags[rng() % slotTags.size()]};
        if (rng() % 4 == 0)
            t.undesiredRoomTags = {roomTags[rng() % roomTags.size()]};
        if (rng() % 5 == 0)
            t.preferredRoomTags = {roomTags[rng() % roomTags.size()]};
        if (rng() % 5 == 0)
            t.prohibitedRoomTags = {roomTags[rng() % roomTags.size()]};
        c.talks.push_back(std::move(t));
    }
    return c;
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves (including unassignments), driven through the ScoreDirector so
// the before/set/after hook contract is exercised exactly like in the engine.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Verifying incremental scoring == full recompute ==\n");
    ConferenceSchedule c = buildRandomInstance(/*talkCount*/ 24, /*seed*/ 7);

    ConferenceSchedulingIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<ConferenceSchedule> director(c, calc);
    ConferenceSchedule shadow = c;

    std::mt19937_64 rng(123);
    const int talkCount = c.talkCount();
    const int valueCount = c.slotRoomCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            // ChangeMove; 10% of the time to unassigned (-1), like the
            // construction heuristic's starting state.
            const int talk = static_cast<int>(rng() % talkCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(talk, value);
        }
        else
        {
            // SwapMove.
            const int a = static_cast<int>(rng() % talkCount);
            const int b = static_cast<int>(rng() % talkCount);
            const int va = c.talks[a].slotRoom;
            const int vb = c.talks[b].slotRoom;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < talkCount; ++i)
            shadow.talks[i].slotRoom = c.talks[i].slotRoom;
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = director.calculateScore();
        if (full.hard != incremental.hard
            || std::fabs(full.soft - incremental.soft) > 1e-9)
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n",
                            step,
                            incremental.toString().c_str(),
                            full.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps)
              + " random moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) FULL_ASSERT solve: the engine cross-checks the incremental score
// against a full reset + recompute after every single move and throws
// ScoreCorruptionError on any divergence.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (c) FULL_ASSERT solve ==\n");
    ConferenceSchedule c = buildRandomInstance(/*talkCount*/ 18, /*seed*/ 11);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    // Kick early so the LNS ruin/recreate path also runs under FULL_ASSERT.
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    bool corrupted = false;
    Score final{};
    try
    {
        Solver solver(config);
        solver.solve(c);
        final = c.score;
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(c);
        check(final.hard == recomputed.hard
                  && std::fabs(final.soft - recomputed.soft) < 1e-9,
              "solver-reported score == full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Pinned talks keep their assignment through a solve; an easy instance
// (distinct speakers, no unavailability, ample rooms) reaches hard 0 with
// every talk assigned.
// -------------------------------------------------------------------------
void verifyPinnedSolve()
{
    std::printf("== (d) Pinned talk survives a solve ==\n");
    ConferenceSchedule c = makeBase();
    // Nine talks with distinct speakers won't exhaust 6 slots x 3 rooms.
    for (int i = 0; i < 9; ++i)
        addTalk(c, ("P" + std::to_string(i)).c_str(), {i % 4});
    const int pinnedValue = c.slotRoomOf(/*slot*/ 3, /*room*/ 2);
    c.talks[0].slotRoom = pinnedValue;
    c.talks[0].pinned = true;

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(1);
    config.randomSeed = 5;
    Solver solver(config);
    solver.solve(c);

    check(c.talks[0].slotRoom == pinnedValue, "pinned talk kept its slotRoom");
    bool allAssigned = true;
    for (const Talk &talk : c.talks)
    {
        if (talk.slotRoom < 0)
            allAssigned = false;
    }
    check(allAssigned, "every talk assigned after the solve");
    check(c.score.hard == 0, "easy instance reaches 0hard");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifyPinnedSolve();

    if (failures == 0)
    {
        std::printf("All conference-scheduling checks PASSED.\n");
        return 0;
    }
    std::printf("%d conference-scheduling check(s) FAILED.\n", failures);
    return 1;
}

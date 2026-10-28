// Plugin-assembly (cs/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 150 random
//       assignments (unassigned talks included)
//   (b) incremental score == full recompute across 400 random moves driven
//       through the engine's ScoreDirector protocol (plugin path)
//   (c) every component in isolation carries exactly its own breakdown term,
//       at its own weight and level (all 37 of them)
//   (d) weight / isHard overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(schedule, calculator) under FULL_ASSERT
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "cs/constraints.hpp"
#include "cs/domain.hpp"
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

bool sameScore(const Score &a, const Score &b)
{
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

Score scoreWith(DecomposedCalculator &calculator, ConferenceSchedule &c)
{
    ScoreCalculator::prepare(c);  // idempotent; the components require it
    calculator.resetWorkingSolution(c);
    return calculator.calculateScore();
}

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
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    ConferenceSchedule c = buildRandomInstance(/*talkCount*/ 24, /*seed*/ 7);
    ScoreCalculator::prepare(c);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int mismatches = 0;
    const int rounds = 150;
    for (int round = 0; round < rounds; ++round)
    {
        for (Talk &talk : c.talks)
        {
            talk.slotRoom = unit(rng) < 0.10
                                ? -1
                                : static_cast<int>(rng() % c.slotRoomCount());
        }
        const Score plugin = scoreWith(calculator, c);
        const Score monolithic = ScoreCalculator::calculateScore(c);
        if (!sameScore(plugin, monolithic))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n",
                            round,
                            plugin.toString().c_str(),
                            monolithic.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(rounds) + " random assignments score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    ConferenceSchedule c = buildRandomInstance(/*talkCount*/ 18, /*seed*/ 11);
    ScoreCalculator::prepare(c);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<ConferenceSchedule> director(c, calculator);
    ConferenceSchedule shadow = c;

    std::mt19937_64 rng(123);
    const int talkCount = c.talkCount();
    const int valueCount = c.slotRoomCount();
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 400;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            const int talk = static_cast<int>(rng() % talkCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(talk, value);
        }
        else
        {
            const int a = static_cast<int>(rng() % talkCount);
            const int b = static_cast<int>(rng() % talkCount);
            const int va = c.talks[a].slotRoom;
            const int vb = c.talks[b].slotRoom;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        for (int i = 0; i < talkCount; ++i)
            shadow.talks[i].slotRoom = c.talks[i].slotRoom;
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = director.calculateScore();
        if (!sameScore(full, incremental))
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
// (c) Every component in isolation carries exactly its own breakdown term:
// the hard ones on the hard level at their own weight, the soft penalties as
// a negative soft, and the three diversity rules as a POSITIVE soft (they are
// rewards).
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Every component in isolation ==\n");
    ConferenceSchedule c = buildRandomInstance(/*talkCount*/ 24, /*seed*/ 13);
    ScoreCalculator::prepare(c);
    std::mt19937_64 rng(5);
    for (Talk &talk : c.talks)
        talk.slotRoom = static_cast<int>(rng() % c.slotRoomCount());
    const Breakdown b = ScoreCalculator::computeBreakdown(c);

    struct Case
    {
        const char *label;
        std::int64_t units;  // the breakdown term
        double weight;       // the component's default weight
        bool hard;
        bool reward;
        void (*add)(DecomposedCalculator &);
    };
    const Case cases[] = {
        {"TalkTypeCompatibility",
         b.talkTypeCompatibility,
         100000.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkTypeCompatibility>(); }},
        {"RoomUnavailableTimeslot",
         b.roomUnavailableTimeslot,
         100000.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<RoomUnavailableTimeslot>(); }},
        {"RoomConflict",
         b.roomConflict,
         1000.0,
         true,
         false,
         [](DecomposedCalculator &calc) { calc.emplace<RoomConflict>(); }},
        {"SpeakerUnavailableTimeslot",
         b.speakerUnavailableTimeslot,
         100.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerUnavailableTimeslot>(); }},
        {"SpeakerConflict",
         b.speakerConflict,
         10.0,
         true,
         false,
         [](DecomposedCalculator &calc) { calc.emplace<SpeakerConflict>(); }},
        {"TalkPrerequisiteTalks",
         b.talkPrerequisiteTalks,
         10.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkPrerequisiteTalks>(); }},
        {"TalkMutuallyExclusiveTalksTags",
         b.talkMutuallyExclusiveTalksTags,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkMutuallyExclusiveTalksTags>(); }},
        {"ConsecutiveTalksPause",
         b.consecutiveTalksPause,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<ConsecutiveTalksPause>(); }},
        {"CrowdControl",
         b.crowdControl,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc) { calc.emplace<CrowdControl>(); }},
        {"SpeakerRequiredTimeslotTags",
         b.speakerRequiredTimeslotTags,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerRequiredTimeslotTags>(); }},
        {"SpeakerProhibitedTimeslotTags",
         b.speakerProhibitedTimeslotTags,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerProhibitedTimeslotTags>(); }},
        {"TalkRequiredTimeslotTags",
         b.talkRequiredTimeslotTags,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkRequiredTimeslotTags>(); }},
        {"TalkProhibitedTimeslotTags",
         b.talkProhibitedTimeslotTags,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkProhibitedTimeslotTags>(); }},
        {"SpeakerRequiredRoomTags",
         b.speakerRequiredRoomTags,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerRequiredRoomTags>(); }},
        {"SpeakerProhibitedRoomTags",
         b.speakerProhibitedRoomTags,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerProhibitedRoomTags>(); }},
        {"TalkRequiredRoomTags",
         b.talkRequiredRoomTags,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkRequiredRoomTags>(); }},
        {"TalkProhibitedRoomTags",
         b.talkProhibitedRoomTags,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkProhibitedRoomTags>(); }},
        {"ThemeTrackConflict",
         b.themeTrackConflict,
         10.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<ThemeTrackConflict>(); }},
        {"ThemeTrackRoomStability",
         b.themeTrackRoomStability,
         10.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<ThemeTrackRoomStability>(); }},
        {"SectorConflict",
         b.sectorConflict,
         10.0,
         false,
         false,
         [](DecomposedCalculator &calc) { calc.emplace<SectorConflict>(); }},
        {"AudienceTypeThemeTrackConflict",
         b.audienceTypeThemeTrackConflict,
         1.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<AudienceTypeThemeTrackConflict>(); }},
        {"ContentAudienceLevelFlowViolation",
         b.contentAudienceLevelFlowViolation,
         10.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<ContentAudienceLevelFlowViolation>(); }},
        {"ContentConflict",
         b.contentConflict,
         100.0,
         false,
         false,
         [](DecomposedCalculator &calc) { calc.emplace<ContentConflict>(); }},
        {"SameDayTalks",
         b.sameDayTalks,
         10.0,
         false,
         false,
         [](DecomposedCalculator &calc) { calc.emplace<SameDayTalks>(); }},
        {"PopularTalks",
         b.popularTalks,
         10.0,
         false,
         false,
         [](DecomposedCalculator &calc) { calc.emplace<PopularTalks>(); }},
        {"SpeakerPreferredTimeslotTags",
         b.speakerPreferredTimeslotTags,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerPreferredTimeslotTags>(); }},
        {"SpeakerUndesiredTimeslotTags",
         b.speakerUndesiredTimeslotTags,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerUndesiredTimeslotTags>(); }},
        {"TalkPreferredTimeslotTags",
         b.talkPreferredTimeslotTags,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkPreferredTimeslotTags>(); }},
        {"TalkUndesiredTimeslotTags",
         b.talkUndesiredTimeslotTags,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkUndesiredTimeslotTags>(); }},
        {"SpeakerPreferredRoomTags",
         b.speakerPreferredRoomTags,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerPreferredRoomTags>(); }},
        {"SpeakerUndesiredRoomTags",
         b.speakerUndesiredRoomTags,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerUndesiredRoomTags>(); }},
        {"TalkPreferredRoomTags",
         b.talkPreferredRoomTags,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkPreferredRoomTags>(); }},
        {"TalkUndesiredRoomTags",
         b.talkUndesiredRoomTags,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkUndesiredRoomTags>(); }},
        {"SpeakerMakespan",
         b.speakerMakespan,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc) { calc.emplace<SpeakerMakespan>(); }},
        {"AudienceTypeDiversity",
         b.audienceTypeDiversity,
         1.0,
         false,
         true,
         [](DecomposedCalculator &calc)
         { calc.emplace<AudienceTypeDiversity>(); }},
        {"AudienceLevelDiversity",
         b.audienceLevelDiversity,
         1.0,
         false,
         true,
         [](DecomposedCalculator &calc)
         { calc.emplace<AudienceLevelDiversity>(); }},
        {"LanguageDiversity",
         b.languageDiversity,
         10.0,
         false,
         true,
         [](DecomposedCalculator &calc) { calc.emplace<LanguageDiversity>(); }},
    };

    int wrong = 0;
    for (const Case &item : cases)
    {
        DecomposedCalculator calculator;
        item.add(calculator);
        const Score score = scoreWith(calculator, c);
        const double expectedHard
            = item.hard ? -item.weight * static_cast<double>(item.units) : 0.0;
        const double expectedSoft
            = item.hard
                  ? 0.0
                  : (item.reward
                         ? item.weight * static_cast<double>(item.units)
                         : -item.weight * static_cast<double>(item.units));
        if (std::fabs(static_cast<double>(score.hard) - expectedHard) > 1e-6
            || std::fabs(score.soft - expectedSoft) > 1e-6)
        {
            ++wrong;
            std::printf("  MISMATCH %-40s got=%s expected=%ghard/%gsoft\n",
                        item.label,
                        score.toString().c_str(),
                        expectedHard,
                        expectedSoft);
        }
    }
    check(wrong == 0,
          std::to_string(sizeof(cases) / sizeof(cases[0]))
              + " components each isolate exactly their breakdown term");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    ConferenceSchedule c = buildRandomInstance(/*talkCount*/ 18, /*seed*/ 17);
    ScoreCalculator::prepare(c);
    std::mt19937_64 rng(9);
    for (Talk &talk : c.talks)
        talk.slotRoom = static_cast<int>(rng() % c.slotRoomCount());
    const Breakdown b = ScoreCalculator::computeBreakdown(c);

    {
        DecomposedCalculator calculator;
        calculator.emplace<RoomConflict>(1.0);
        const Score score = scoreWith(calculator, c);
        check(score.hard == -b.roomConflict, "roomConflict re-weighted to 1");
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<RoomConflict>(1.0);
        component->isHard = false;
        calculator.add(std::move(component));
        const Score score = scoreWith(calculator, c);
        check(score.hard == 0
                  && std::fabs(score.soft + static_cast<double>(b.roomConflict))
                         < 1e-6,
              "roomConflict demoted to soft");
    }
    {
        // A reward re-weighted stays a reward: the soft level goes UP.
        DecomposedCalculator calculator;
        calculator.emplace<LanguageDiversity>(20.0);
        const Score score = scoreWith(calculator, c);
        check(std::fabs(score.soft
                        - 20.0 * static_cast<double>(b.languageDiversity))
                  < 1e-6,
              "languageDiversity stays a reward when re-weighted");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    ConferenceSchedule c = buildRandomInstance(/*talkCount*/ 18, /*seed*/ 19);
    ScoreCalculator::prepare(c);
    std::mt19937_64 rng(5);
    for (Talk &talk : c.talks)
        talk.slotRoom = static_cast<int>(rng() % c.slotRoomCount());

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    const Score score = scoreWith(calculator, c);

    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-6
              && std::fabs(-soft - score.soft) < 1e-6,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == 37,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    ConferenceSchedule c = buildRandomInstance(/*talkCount*/ 12, /*seed*/ 11);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    bool corrupted = false;
    try
    {
        Solver solver(config);
        solver.solve(c, calculator);
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(c);
        check(sameScore(c.score, recomputed),
              "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyAssemblyParity();
    verifyIncremental();
    verifyPartialAssemblies();
    verifyOverrides();
    verifyConstraintTotals();
    verifyFullAssertSolve();

    if (failures == 0)
    {
        std::printf("All conference-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d conference-scheduling plugin check(s) FAILED.\n", failures);
    return 1;
}

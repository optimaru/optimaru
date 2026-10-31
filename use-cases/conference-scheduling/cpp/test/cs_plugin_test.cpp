// Plugin-assembly (cs/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 150 random
//       assignments (unassigned talks included)
//   (b) incremental score == full recompute across 400 random moves driven
//       through the engine's ScoreDirector protocol (plugin path)
//   (c) every component in isolation carries exactly its own breakdown term,
//       at its own weight and level (all 37 of them), and the same table is
//       the catalog: constraintCosts() metadata and defaultConstraints().info()
//       rows equal it, in catalog order
//   (d) weight / level overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(schedule, calculator) under FULL_ASSERT
//   (g) each component's raw cost, hand-calculated through constraintCosts():
//       a positive and a zero case per constraint (raw 0 = no violation; the
//       three rewards report raw <= 0)
//   (h) the knob guards: the three rewards refuse hard() and any level
//       change, every other constraint flips both ways
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
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
// rewards). The same table is the catalog: each component's constraintCosts()
// metadata and the defaultConstraints().info() row at the same position.
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

    // One row per component, in catalog order (defaultConstraints()); the
    // label is the struct name, the weight / hard / reward columns its
    // defaults (checked against constraintCosts() and info() below).
    struct Case
    {
        const char *label;
        std::int64_t units;  // the Breakdown member of the same name
        double weight;       // the component's kDefaultWeight
        bool hard;           // kDefaultLevel == Level::Hard
        bool reward;
        void (*add)(DecomposedCalculator &);
    };
    const Case cases[] = {
        {"TalkTypeIncompatibilityCost",
         b.talkTypeIncompatibilityCost,
         100000.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkTypeIncompatibilityCost>(); }},
        {"RoomUnavailabilityCost",
         b.roomUnavailabilityCost,
         100000.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<RoomUnavailabilityCost>(); }},
        {"RoomConflictCost",
         b.roomConflictCost,
         1000.0,
         true,
         false,
         [](DecomposedCalculator &calc) { calc.emplace<RoomConflictCost>(); }},
        {"SpeakerUnavailabilityCost",
         b.speakerUnavailabilityCost,
         100.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerUnavailabilityCost>(); }},
        {"SpeakerConflictCost",
         b.speakerConflictCost,
         10.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerConflictCost>(); }},
        {"TalkPrerequisiteTalksCost",
         b.talkPrerequisiteTalksCost,
         10.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkPrerequisiteTalksCost>(); }},
        {"TalkMutuallyExclusiveTalksTagsCost",
         b.talkMutuallyExclusiveTalksTagsCost,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkMutuallyExclusiveTalksTagsCost>(); }},
        {"ConsecutiveTalksPauseCost",
         b.consecutiveTalksPauseCost,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<ConsecutiveTalksPauseCost>(); }},
        {"CrowdControlRiskCost",
         b.crowdControlRiskCost,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<CrowdControlRiskCost>(); }},
        {"SpeakerRequiredTimeslotTagsCost",
         b.speakerRequiredTimeslotTagsCost,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerRequiredTimeslotTagsCost>(); }},
        {"SpeakerProhibitedTimeslotTagsCost",
         b.speakerProhibitedTimeslotTagsCost,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerProhibitedTimeslotTagsCost>(); }},
        {"TalkRequiredTimeslotTagsCost",
         b.talkRequiredTimeslotTagsCost,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkRequiredTimeslotTagsCost>(); }},
        {"TalkProhibitedTimeslotTagsCost",
         b.talkProhibitedTimeslotTagsCost,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkProhibitedTimeslotTagsCost>(); }},
        {"SpeakerRequiredRoomTagsCost",
         b.speakerRequiredRoomTagsCost,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerRequiredRoomTagsCost>(); }},
        {"SpeakerProhibitedRoomTagsCost",
         b.speakerProhibitedRoomTagsCost,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerProhibitedRoomTagsCost>(); }},
        {"TalkRequiredRoomTagsCost",
         b.talkRequiredRoomTagsCost,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkRequiredRoomTagsCost>(); }},
        {"TalkProhibitedRoomTagsCost",
         b.talkProhibitedRoomTagsCost,
         1.0,
         true,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkProhibitedRoomTagsCost>(); }},
        {"ThemeTrackConflictCost",
         b.themeTrackConflictCost,
         10.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<ThemeTrackConflictCost>(); }},
        {"ThemeTrackRoomChangeCost",
         b.themeTrackRoomChangeCost,
         10.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<ThemeTrackRoomChangeCost>(); }},
        {"SectorConflictCost",
         b.sectorConflictCost,
         10.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SectorConflictCost>(); }},
        {"AudienceTypeThemeTrackConflictCost",
         b.audienceTypeThemeTrackConflictCost,
         1.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<AudienceTypeThemeTrackConflictCost>(); }},
        {"ContentAudienceLevelFlowViolationCost",
         b.contentAudienceLevelFlowViolationCost,
         10.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<ContentAudienceLevelFlowViolationCost>(); }},
        {"ContentConflictCost",
         b.contentConflictCost,
         100.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<ContentConflictCost>(); }},
        {"DifferentDayTalksCost",
         b.differentDayTalksCost,
         10.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<DifferentDayTalksCost>(); }},
        {"PopularTalksRoomSizeCost",
         b.popularTalksRoomSizeCost,
         10.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<PopularTalksRoomSizeCost>(); }},
        {"SpeakerPreferredTimeslotTagsCost",
         b.speakerPreferredTimeslotTagsCost,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerPreferredTimeslotTagsCost>(); }},
        {"SpeakerUndesiredTimeslotTagsCost",
         b.speakerUndesiredTimeslotTagsCost,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerUndesiredTimeslotTagsCost>(); }},
        {"TalkPreferredTimeslotTagsCost",
         b.talkPreferredTimeslotTagsCost,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkPreferredTimeslotTagsCost>(); }},
        {"TalkUndesiredTimeslotTagsCost",
         b.talkUndesiredTimeslotTagsCost,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkUndesiredTimeslotTagsCost>(); }},
        {"SpeakerPreferredRoomTagsCost",
         b.speakerPreferredRoomTagsCost,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerPreferredRoomTagsCost>(); }},
        {"SpeakerUndesiredRoomTagsCost",
         b.speakerUndesiredRoomTagsCost,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerUndesiredRoomTagsCost>(); }},
        {"TalkPreferredRoomTagsCost",
         b.talkPreferredRoomTagsCost,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkPreferredRoomTagsCost>(); }},
        {"TalkUndesiredRoomTagsCost",
         b.talkUndesiredRoomTagsCost,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<TalkUndesiredRoomTagsCost>(); }},
        {"SpeakerMakespanCost",
         b.speakerMakespanCost,
         20.0,
         false,
         false,
         [](DecomposedCalculator &calc)
         { calc.emplace<SpeakerMakespanCost>(); }},
        {"AudienceTypeDiversityCost",
         b.audienceTypeDiversityCost,
         1.0,
         false,
         true,
         [](DecomposedCalculator &calc)
         { calc.emplace<AudienceTypeDiversityCost>(); }},
        {"AudienceLevelDiversityCost",
         b.audienceLevelDiversityCost,
         1.0,
         false,
         true,
         [](DecomposedCalculator &calc)
         { calc.emplace<AudienceLevelDiversityCost>(); }},
        {"LanguageDiversityCost",
         b.languageDiversityCost,
         10.0,
         false,
         true,
         [](DecomposedCalculator &calc)
         { calc.emplace<LanguageDiversityCost>(); }},
    };

    const std::vector<optimaru::ConstraintInfo> rows
        = defaultConstraints().info();
    int wrong = 0;
    int wrongMetadata = 0;
    std::size_t index = 0;
    for (const Case &item : cases)
    {
        DecomposedCalculator calculator;
        item.add(calculator);
        const Score score = scoreWith(calculator, c);

        // The component's own metadata (constraintCosts) and the catalog row
        // at the same position equal the table: registry name = the struct
        // name with a lower-case first letter, default weight and level, the
        // reward flag, flippable unless a reward, raw = +-units.
        std::string name = item.label;
        name[0] = static_cast<char>(name[0] - 'A' + 'a');
        const auto entries = calculator.constraintCosts();
        const double raw = item.reward ? -static_cast<double>(item.units)
                                       : static_cast<double>(item.units);
        const bool entryOk
            = entries.size() == 1 && entries[0].name == name
              && entries[0].weight == item.weight
              && entries[0].isHard == item.hard
              && entries[0].level
                     == (item.hard ? optimaru::Level::Hard
                                   : optimaru::Level::Soft)
              && entries[0].reward == item.reward
              && entries[0].levelFlippable == !item.reward
              && entries[0].isGlobal
              && entries[0].unitCosts == std::vector<double>{raw};
        const bool rowOk
            = index < rows.size() && rows[index].name == name
              && rows[index].defaultWeight == item.weight
              && (rows[index].defaultLevel == optimaru::Level::Hard)
                     == item.hard
              && rows[index].reward == item.reward
              && rows[index].levelFlippable == !item.reward
              && rows[index].isGlobal && rows[index].defaultEnabled;
        ++index;
        if (!entryOk || !rowOk)
        {
            ++wrongMetadata;
            std::printf("  METADATA MISMATCH %s (entry %d, catalog row %d)\n",
                        item.label,
                        entryOk ? 1 : 0,
                        rowOk ? 1 : 0);
        }
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
    check(wrongMetadata == 0 && rows.size() == index,
          "constraintCosts() metadata and the info() catalog rows equal the "
          "table, in catalog order ("
              + std::to_string(rows.size()) + " rows)");
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
        calculator.emplace<RoomConflictCost>(1.0);
        const Score score = scoreWith(calculator, c);
        check(score.hard == -b.roomConflictCost,
              "roomConflictCost re-weighted to 1");
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<RoomConflictCost>(1.0);
        component->level = optimaru::Level::Soft;
        calculator.add(std::move(component));
        const Score score = scoreWith(calculator, c);
        check(score.hard == 0
                  && std::fabs(score.soft
                               + static_cast<double>(b.roomConflictCost))
                         < 1e-6,
              "roomConflictCost demoted to soft");
    }
    {
        // A reward re-weighted stays a reward: the soft level goes UP.
        DecomposedCalculator calculator;
        calculator.emplace<LanguageDiversityCost>(20.0);
        const Score score = scoreWith(calculator, c);
        check(std::fabs(score.soft
                        - 20.0 * static_cast<double>(b.languageDiversityCost))
                  < 1e-6,
              "languageDiversityCost stays a reward when re-weighted");
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

// -------------------------------------------------------------------------
// (g) Each component's raw cost on hand-built instances, read through
// constraintCosts() — the raw numbers before weight, exponent and level. Every
// fixture states the non-zero raw costs it expects; every other constraint of
// the default assembly must read exactly 0 on it. Each constraint gets one
// violating (positive) fixture and one near-miss (zero) fixture.
//
// Base: one talk type "B" (0), three rooms (capacity 100 / 200 / 300) and six
// timeslots (minutes; day = minute / 1440):
//   s0 540-600 (day 0)          s1 540-600 (day 0, distinct slot, same time)
//   s2 570-630 (overlaps s0/s1 by 30)
//   s3 630-690 (30 minutes after s0, touches s2)
//   s4 1980-2040 (day 1)        s5 3420-3480 (day 2)
// -------------------------------------------------------------------------
ConferenceSchedule rawBase()
{
    ConferenceSchedule c;
    c.name = "raw";
    c.talkTypes = {"B"};
    const std::int64_t times[6][2] = {{540, 600},
                                      {540, 600},
                                      {570, 630},
                                      {630, 690},
                                      {1980, 2040},
                                      {3420, 3480}};
    for (int s = 0; s < 6; ++s)
    {
        Timeslot t;
        t.id = "s" + std::to_string(s);
        t.startMinute = times[s][0];
        t.endMinute = times[s][1];
        t.talkTypes = {0};
        c.timeslots.push_back(std::move(t));
    }
    for (int r = 0; r < 3; ++r)
    {
        Room room;
        room.id = "r" + std::to_string(r);
        room.capacity = 100 * (r + 1);
        room.talkTypes = {0};
        c.rooms.push_back(std::move(room));
    }
    for (int k = 0; k < 4; ++k)
    {
        Speaker speaker;
        speaker.id = "k" + std::to_string(k);
        speaker.name = speaker.id;
        c.speakers.push_back(std::move(speaker));
    }
    return c;
}

Talk &
placeTalk(ConferenceSchedule &c, std::vector<int> speakers, int slot, int room)
{
    Talk t;
    t.code = "T" + std::to_string(c.talks.size());
    t.talkType = 0;
    t.speakers = std::move(speakers);
    t.language = "en";
    t.slotRoom = c.slotRoomOf(slot, room);
    c.talks.push_back(std::move(t));
    return c.talks.back();
}

// Raw cost of every default constraint (all global: one value each), by
// registry name.
std::map<std::string, double> rawCosts(ConferenceSchedule &c)
{
    ScoreCalculator::prepare(c);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(c);
    calculator.calculateScore();
    std::map<std::string, double> raw;
    for (const auto &entry : calculator.constraintCosts())
        raw[entry.name] = entry.unitCosts.at(0);
    return raw;
}

void checkRaw(ConferenceSchedule c,
              const std::map<std::string, double> &expected,
              const std::string &label)
{
    const std::map<std::string, double> raw = rawCosts(c);
    bool ok = raw.size() == 37;
    for (const auto &[name, value] : expected)
        ok = ok && raw.count(name) == 1;  // a misspelt name fails too
    for (const auto &[name, value] : raw)
    {
        const auto it = expected.find(name);
        const double want = it == expected.end() ? 0.0 : it->second;
        if (value != want)
        {
            ok = false;
            std::printf(
                "    %s: raw %g, expected %g\n", name.c_str(), value, want);
        }
    }
    check(ok, label);
}

void verifyRawCostsByHand()
{
    std::printf("== (g) Raw costs by hand, positive and zero ==\n");
    {
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0}, 0, 0);
        placeTalk(c, {1}, 3, 1);
        checkRaw(c, {}, "a quiet schedule: all 37 raw costs are 0");
    }
    // talkTypeIncompatibilityCost: an "L" talk in a "B"-only slot and room
    // violates both halves, max(60, 1) each; admitted everywhere -> 0.
    {
        ConferenceSchedule c = rawBase();
        c.talkTypes.push_back("L");
        placeTalk(c, {0}, 0, 0).talkType = 1;
        checkRaw(c,
                 {{"talkTypeIncompatibilityCost", 120.0}},
                 "talkTypeIncompatibilityCost: 2 x 60 (slot and room)");
        c.timeslots[0].talkTypes = {0, 1};
        c.rooms[0].talkTypes = {0, 1};
        checkRaw(c, {}, "talkTypeIncompatibilityCost: 0 once both admit L");
    }
    // roomUnavailabilityCost: r0 is closed in s0.
    {
        ConferenceSchedule c = rawBase();
        c.rooms[0].unavailableTimeslots = {0};
        placeTalk(c, {0}, 0, 0);
        checkRaw(c,
                 {{"roomUnavailabilityCost", 60.0}},
                 "roomUnavailabilityCost: 60 in the closed slot");
        c.talks[0].slotRoom = c.slotRoomOf(3, 0);
        checkRaw(c, {}, "roomUnavailabilityCost: 0 in an open slot");
    }
    // roomConflictCost: s0 / s2 overlap by 30 minutes in r0.
    {
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0}, 0, 0);
        placeTalk(c, {1}, 2, 0);
        checkRaw(c,
                 {{"roomConflictCost", 30.0}},
                 "roomConflictCost: 30 overlapping minutes");
        c.talks[1].slotRoom = c.slotRoomOf(3, 0);
        checkRaw(c, {}, "roomConflictCost: 0 back to back in one room");
    }
    // speakerUnavailabilityCost: both speakers unavailable in s0.
    {
        ConferenceSchedule c = rawBase();
        c.speakers[0].unavailableTimeslots = {0};
        c.speakers[1].unavailableTimeslots = {0};
        placeTalk(c, {0, 1}, 0, 0);
        checkRaw(c,
                 {{"speakerUnavailabilityCost", 120.0}},
                 "speakerUnavailabilityCost: 2 speakers x 60");
        c.speakers[0].unavailableTimeslots = {1};
        c.speakers[1].unavailableTimeslots = {1};
        checkRaw(c, {}, "speakerUnavailabilityCost: 0 when they are free");
    }
    // speakerConflictCost: two shared speakers, 60 minutes of overlap; the
    // pair also lacks the consecutive pause (combined 120).
    {
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0, 1}, 0, 0);
        placeTalk(c, {0, 1}, 1, 1);
        checkRaw(c,
                 {{"speakerConflictCost", 120.0},
                  {"consecutiveTalksPauseCost", 120.0}},
                 "speakerConflictCost: 2 shared x 60 (+ pause 120)");
        c.talks[1].slotRoom = c.slotRoomOf(4, 1);  // the next day
        checkRaw(c, {}, "speakerConflictCost: 0 on different days");
    }
    // talkPrerequisiteTalksCost: the prerequisite (s3) ends after its
    // dependent (s0) starts: combined 120.
    {
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0}, 3, 0);
        placeTalk(c, {1}, 0, 1).prerequisiteTalks = {0};
        checkRaw(c,
                 {{"talkPrerequisiteTalksCost", 120.0}},
                 "talkPrerequisiteTalksCost: combined 120");
        c.talks[0].slotRoom = c.slotRoomOf(0, 0);
        c.talks[1].slotRoom = c.slotRoomOf(3, 1);
        checkRaw(c, {}, "talkPrerequisiteTalksCost: 0 in order");
    }
    // talkMutuallyExclusiveTalksTagsCost: 2 shared tags x 60 overlap.
    {
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0}, 0, 0).mutuallyExclusiveTalksTags = {"M", "N"};
        placeTalk(c, {1}, 1, 1).mutuallyExclusiveTalksTags = {"M", "N"};
        checkRaw(c,
                 {{"talkMutuallyExclusiveTalksTagsCost", 120.0}},
                 "talkMutuallyExclusiveTalksTagsCost: 2 x 60");
        c.talks[1].slotRoom = c.slotRoomOf(3, 1);
        checkRaw(c, {}, "talkMutuallyExclusiveTalksTagsCost: 0 apart");
    }
    // consecutiveTalksPauseCost: s2 ends where s3 starts (gap 0 < 30).
    {
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0}, 2, 0);
        placeTalk(c, {0}, 3, 1);
        checkRaw(c,
                 {{"consecutiveTalksPauseCost", 120.0}},
                 "consecutiveTalksPauseCost: gap 0, combined 120");
        c.talks[0].slotRoom = c.slotRoomOf(0, 0);  // gap 30 == the minimum
        checkRaw(c, {}, "consecutiveTalksPauseCost: 0 with a 30-minute gap");
    }
    // crowdControlRiskCost: three risky talks in s0 -> 3 x 60; two are fine.
    {
        ConferenceSchedule c = rawBase();
        for (int r = 0; r < 3; ++r)
            placeTalk(c, {r}, 0, r).crowdControlRisk = 1;
        checkRaw(c,
                 {{"crowdControlRiskCost", 180.0}},
                 "crowdControlRiskCost: three risky talks, 3 x 60");
        c.talks[2].crowdControlRisk = 0;
        checkRaw(c, {}, "crowdControlRiskCost: 0 with two risky talks");
    }
    // The sixteen tag constraints: s0 carries {TS}, r0 carries {RM}. Each
    // missing (required / preferred) or present (prohibited / undesired) tag
    // costs the slot's 60 minutes; k1 misses two required room tags.
    {
        ConferenceSchedule c = rawBase();
        c.timeslots[0].tags = {"TS"};
        c.rooms[0].tags = {"RM"};
        Talk &t = placeTalk(c, {0, 1}, 0, 0);
        t.requiredTimeslotTags = {"TS", "X"};
        t.prohibitedTimeslotTags = {"TS"};
        t.preferredTimeslotTags = {"Y"};
        t.undesiredTimeslotTags = {"TS"};
        t.requiredRoomTags = {"RM", "Z"};
        t.prohibitedRoomTags = {"RM"};
        t.preferredRoomTags = {"W"};
        t.undesiredRoomTags = {"RM"};
        Speaker &k0 = c.speakers[0];
        k0.requiredTimeslotTags = {"X"};
        k0.prohibitedTimeslotTags = {"TS"};
        k0.preferredTimeslotTags = {"TS", "Y"};
        k0.undesiredTimeslotTags = {"TS"};
        Speaker &k1 = c.speakers[1];
        k1.requiredRoomTags = {"Z", "W"};
        k1.prohibitedRoomTags = {"RM"};
        k1.preferredRoomTags = {"W"};
        k1.undesiredRoomTags = {"RM"};
        checkRaw(c,
                 {{"talkRequiredTimeslotTagsCost", 60.0},
                  {"talkProhibitedTimeslotTagsCost", 60.0},
                  {"talkPreferredTimeslotTagsCost", 60.0},
                  {"talkUndesiredTimeslotTagsCost", 60.0},
                  {"talkRequiredRoomTagsCost", 60.0},
                  {"talkProhibitedRoomTagsCost", 60.0},
                  {"talkPreferredRoomTagsCost", 60.0},
                  {"talkUndesiredRoomTagsCost", 60.0},
                  {"speakerRequiredTimeslotTagsCost", 60.0},
                  {"speakerProhibitedTimeslotTagsCost", 60.0},
                  {"speakerPreferredTimeslotTagsCost", 60.0},
                  {"speakerUndesiredTimeslotTagsCost", 60.0},
                  {"speakerRequiredRoomTagsCost", 120.0},
                  {"speakerProhibitedRoomTagsCost", 60.0},
                  {"speakerPreferredRoomTagsCost", 60.0},
                  {"speakerUndesiredRoomTagsCost", 60.0}},
                 "the 16 tag constraints: 60 each, 120 for 2 missing tags");
    }
    {
        // Every tag wish met: required/preferred present, the prohibited and
        // undesired ones absent.
        ConferenceSchedule c = rawBase();
        c.timeslots[0].tags = {"TS"};
        c.rooms[0].tags = {"RM"};
        Talk &t = placeTalk(c, {0}, 0, 0);
        Speaker &k = c.speakers[0];
        t.requiredTimeslotTags = k.requiredTimeslotTags = {"TS"};
        t.preferredTimeslotTags = k.preferredTimeslotTags = {"TS"};
        t.prohibitedTimeslotTags = k.prohibitedTimeslotTags = {"X"};
        t.undesiredTimeslotTags = k.undesiredTimeslotTags = {"X"};
        t.requiredRoomTags = k.requiredRoomTags = {"RM"};
        t.preferredRoomTags = k.preferredRoomTags = {"RM"};
        t.prohibitedRoomTags = k.prohibitedRoomTags = {"X"};
        t.undesiredRoomTags = k.undesiredRoomTags = {"X"};
        checkRaw(c, {}, "the 16 tag constraints: 0 when every wish is met");
    }
    // Theme / audience on one overlapping same-day pair in different rooms:
    // themeTrackConflictCost 1 x 60, themeTrackRoomChangeCost 1 x 120,
    // audienceTypeThemeTrackConflictCost 1 x 1 x 60.
    {
        ConferenceSchedule c = rawBase();
        for (int slot = 0; slot < 2; ++slot)
        {
            Talk &t = placeTalk(c, {slot}, slot, slot);
            t.themeTrackTags = {"AI"};
            t.audienceTypes = {"Dev"};
        }
        checkRaw(c,
                 {{"themeTrackConflictCost", 60.0},
                  {"themeTrackRoomChangeCost", 120.0},
                  {"audienceTypeThemeTrackConflictCost", 60.0}},
                 "theme conflict 60, room change 120, audience x theme 60");
        c.talks[1].themeTrackTags = {"Cloud"};
        checkRaw(c, {}, "the theme constraints: 0 on different themes");
    }
    {
        // Same theme, same day, same room, back to back: no room change.
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0}, 0, 0).themeTrackTags = {"AI"};
        placeTalk(c, {1}, 3, 0).themeTrackTags = {"AI"};
        checkRaw(c, {}, "themeTrackRoomChangeCost: 0 in one room");
    }
    // sectorConflictCost: 2 shared sectors x 30 minutes of overlap.
    {
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0}, 0, 0).sectorTags = {"Fin", "Gov"};
        placeTalk(c, {1}, 2, 1).sectorTags = {"Fin", "Gov"};
        checkRaw(c,
                 {{"sectorConflictCost", 60.0}},
                 "sectorConflictCost: 2 sectors x 30");
        c.talks[1].slotRoom = c.slotRoomOf(3, 1);
        checkRaw(c, {}, "sectorConflictCost: 0 without overlap");
    }
    // contentAudienceLevelFlowViolationCost: the level-1 talk (s3) ends after
    // the level-2 talk (s0) starts; combined 120.
    {
        ConferenceSchedule c = rawBase();
        Talk &low = placeTalk(c, {0}, 3, 0);
        low.audienceLevel = 1;
        low.contentTags = {"CT"};
        Talk &high = placeTalk(c, {1}, 0, 1);
        high.audienceLevel = 2;
        high.contentTags = {"CT"};
        checkRaw(c,
                 {{"contentAudienceLevelFlowViolationCost", 120.0}},
                 "contentAudienceLevelFlowViolationCost: combined 120");
        c.talks[0].slotRoom = c.slotRoomOf(0, 0);
        c.talks[1].slotRoom = c.slotRoomOf(3, 1);
        checkRaw(c, {}, "contentAudienceLevelFlowViolationCost: 0 in order");
    }
    // contentConflictCost: one shared content x 60 overlap (equal levels).
    {
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0}, 0, 0).contentTags = {"CT"};
        placeTalk(c, {1}, 1, 1).contentTags = {"CT"};
        checkRaw(c, {{"contentConflictCost", 60.0}}, "contentConflictCost: 60");
        c.talks[1].slotRoom = c.slotRoomOf(3, 1);
        checkRaw(c, {}, "contentConflictCost: 0 without overlap");
    }
    // differentDayTalksCost: shared content + theme on days 0 and 1:
    // (1 + 1) x combined 120.
    {
        ConferenceSchedule c = rawBase();
        for (int k = 0; k < 2; ++k)
        {
            Talk &t = placeTalk(c, {k}, k == 0 ? 0 : 4, k);
            t.contentTags = {"CT"};
            t.themeTrackTags = {"AI"};
        }
        checkRaw(c,
                 {{"differentDayTalksCost", 240.0}},
                 "differentDayTalksCost: (1 + 1) x 120");
        c.talks[1].slotRoom = c.slotRoomOf(3, 0);  // same day, same room
        checkRaw(c, {}, "differentDayTalksCost: 0 on the same day");
    }
    // popularTalksRoomSizeCost: the talk with fewer favorites (10) has the
    // larger room (300 vs 100); combined 120.
    {
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0}, 0, 2).favoriteCount = 10;
        placeTalk(c, {1}, 3, 0).favoriteCount = 20;
        checkRaw(c,
                 {{"popularTalksRoomSizeCost", 120.0}},
                 "popularTalksRoomSizeCost: combined 120");
        c.talks[0].slotRoom = c.slotRoomOf(0, 0);
        c.talks[1].slotRoom = c.slotRoomOf(3, 2);
        checkRaw(c, {}, "popularTalksRoomSizeCost: 0 when sizes follow");
    }
    // speakerMakespanCost: one speaker on days 0 and 2 -> (2 - 1) x 480.
    {
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0}, 0, 0);
        placeTalk(c, {0}, 5, 1);
        checkRaw(c,
                 {{"speakerMakespanCost", 480.0}},
                 "speakerMakespanCost: (2 - 1) x 480");
        c.talks[1].slotRoom = c.slotRoomOf(4, 1);  // days 0 and 1
        checkRaw(c, {}, "speakerMakespanCost: 0 within two days");
    }
    // The three rewards on one same-slot pair: a shared audience type, two
    // levels and two languages -> raw -60 each (raw <= 0, a reward).
    {
        ConferenceSchedule c = rawBase();
        Talk &a = placeTalk(c, {0}, 0, 0);
        a.audienceTypes = {"Dev"};
        a.audienceLevel = 1;
        Talk &b = placeTalk(c, {1}, 0, 1);
        b.audienceTypes = {"Dev"};
        b.audienceLevel = 2;
        b.language = "de";
        checkRaw(c,
                 {{"audienceTypeDiversityCost", -60.0},
                  {"audienceLevelDiversityCost", -60.0},
                  {"languageDiversityCost", -60.0}},
                 "the 3 diversity rewards: raw -60 each");
        // Same slot, but no shared audience, one level, one language.
        c.talks[1].audienceTypes = {"Ops"};
        c.talks[1].audienceLevel = 1;
        c.talks[1].language = "en";
        checkRaw(c, {}, "the 3 diversity rewards: 0 when nothing is earned");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) The knob guards on the domain's assembly. The three *DiversityCost
// rewards (raw <= 0) refuse hard() and every level change; re-stating their
// soft level is a no-op. Every other constraint flips both ways.
// -------------------------------------------------------------------------
bool throwsInvalid(const std::function<void()> &edit)
{
    try
    {
        edit();
    }
    catch (const std::invalid_argument &)
    {
        return true;
    }
    return false;
}

void verifyGuards()
{
    std::printf("== (h) Reward and knob guards ==\n");
    const char *rewards[] = {"audienceTypeDiversityCost",
                             "audienceLevelDiversityCost",
                             "languageDiversityCost"};
    bool rewardsRefuse = true;
    for (const char *name : rewards)
    {
        const std::string n = name;
        rewardsRefuse
            = rewardsRefuse
              && throwsInvalid([&] { defaultConstraints().hard(n); })
              && throwsInvalid(
                  [&]
                  { defaultConstraints().level(n, optimaru::Level::Medium); })
              && !throwsInvalid([&] { defaultConstraints().soft(n); });
    }
    check(rewardsRefuse,
          "the 3 rewards refuse hard() and medium; soft() is a no-op");
    {
        LanguageDiversityCost component;
        check(component.reward && !component.levelFlippable
                  && throwsInvalid(
                      [&] { component.setLevel(optimaru::Level::Hard); }),
              "a reward component refuses setLevel(Hard) itself");
    }
    int flips = 0;
    bool othersFlip = true;
    for (const optimaru::ConstraintInfo &row : defaultConstraints().info())
    {
        if (row.reward)
            continue;
        ++flips;
        othersFlip
            = othersFlip && row.levelFlippable
              && !throwsInvalid([&] { defaultConstraints().soft(row.name); })
              && !throwsInvalid([&] { defaultConstraints().hard(row.name); });
    }
    check(othersFlip && flips == 34, "the other 34 constraints flip both ways");
    {
        // A demoted hard constraint scores on the soft level at its weight.
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0}, 0, 0);
        placeTalk(c, {1}, 2, 0);  // roomConflictCost raw 30
        ScoreCalculator::prepare(c);
        DecomposedCalculator calculator;
        defaultConstraints().soft("roomConflictCost").into(calculator);
        const Score score = scoreWith(calculator, c);
        check(
            score.hard == 0
                && std::fabs(score.soft
                             + RoomConflictCost::kDefaultWeight * 30.0)
                       < 1e-9,
            "soft(\"roomConflictCost\"): 30 raw -> soft at the default weight");
    }
    check(throwsInvalid(
              [] { defaultConstraints().weight("roomConflictCost", -1.0); })
              && throwsInvalid(
                  []
                  { defaultConstraints().exponent("roomConflictCost", 0.0); }),
          "a negative weight and a non-positive exponent are refused");
    check(
        throwsInvalid([] { defaultConstraints().weight("roomConflict", 2.0); }),
        "a pre-0.2.0 name is unknown");
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
    verifyRawCostsByHand();
    verifyGuards();

    if (failures == 0)
    {
        std::printf("All conference-scheduling plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d conference-scheduling plugin check(s) FAILED.\n", failures);
    return 1;
}

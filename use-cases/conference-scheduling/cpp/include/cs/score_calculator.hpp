#ifndef CS_SCORE_CALCULATOR_HPP
#define CS_SCORE_CALCULATOR_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cs/domain.hpp"

namespace cs
{

// Per-constraint match-unit totals of one full recompute. Each field is the
// sum of the Java constraint's matchWeigher over all matches (all counted
// positive); the constraint weights are applied only in hardTotal() /
// softPenaltyTotal() / softRewardTotal(), mirroring
// `penalize(HardSoftScore.ofHard(w), weigher)` one to one.
struct Breakdown
{
    // Hard units.
    std::int64_t talkTypeCompatibility = 0;       // synthetic; see note below
    std::int64_t roomUnavailableTimeslot = 0;     // duration
    std::int64_t roomConflict = 0;                // overlap minutes
    std::int64_t speakerUnavailableTimeslot = 0;  // duration per speaker
    std::int64_t speakerConflict = 0;  // overlap minutes per shared speaker
    std::int64_t talkPrerequisiteTalks = 0;           // combined duration
    std::int64_t talkMutuallyExclusiveTalksTags = 0;  // tags x overlap
    std::int64_t consecutiveTalksPause = 0;           // combined duration
    std::int64_t crowdControl = 0;                    // duration
    std::int64_t speakerRequiredTimeslotTags = 0;     // missing x duration
    std::int64_t speakerProhibitedTimeslotTags = 0;   // prevailing x duration
    std::int64_t talkRequiredTimeslotTags = 0;
    std::int64_t talkProhibitedTimeslotTags = 0;
    std::int64_t speakerRequiredRoomTags = 0;
    std::int64_t speakerProhibitedRoomTags = 0;
    std::int64_t talkRequiredRoomTags = 0;
    std::int64_t talkProhibitedRoomTags = 0;

    // Soft penalty units.
    std::int64_t themeTrackConflict = 0;       // themes x overlap
    std::int64_t themeTrackRoomStability = 0;  // themes x combined duration
    std::int64_t sectorConflict = 0;           // sectors x overlap
    std::int64_t audienceTypeThemeTrackConflict = 0;     // th x aud x overlap
    std::int64_t contentAudienceLevelFlowViolation = 0;  // content x combined
    std::int64_t contentConflict = 0;                    // content x overlap
    std::int64_t sameDayTalks = 0;  // (themes + content) x combined duration
    std::int64_t popularTalks = 0;  // combined duration
    std::int64_t speakerPreferredTimeslotTags = 0;  // missing x duration
    std::int64_t speakerUndesiredTimeslotTags = 0;  // prevailing x duration
    std::int64_t talkPreferredTimeslotTags = 0;
    std::int64_t talkUndesiredTimeslotTags = 0;
    std::int64_t speakerPreferredRoomTags = 0;
    std::int64_t speakerUndesiredRoomTags = 0;
    std::int64_t talkPreferredRoomTags = 0;
    std::int64_t talkUndesiredRoomTags = 0;
    std::int64_t speakerMakespan = 0;  // (day span - 1) x 8h per speaker

    // Soft reward units.
    std::int64_t audienceTypeDiversity = 0;   // audiences x slot duration
    std::int64_t audienceLevelDiversity = 0;  // slot duration
    std::int64_t languageDiversity = 0;       // slot duration

    std::int64_t hardTotal() const
    {
        return 100000 * (talkTypeCompatibility + roomUnavailableTimeslot)
               + 1000 * roomConflict + 100 * speakerUnavailableTimeslot
               + 10 * (speakerConflict + talkPrerequisiteTalks)
               + talkMutuallyExclusiveTalksTags + consecutiveTalksPause
               + crowdControl + speakerRequiredTimeslotTags
               + speakerProhibitedTimeslotTags + talkRequiredTimeslotTags
               + talkProhibitedTimeslotTags + speakerRequiredRoomTags
               + speakerProhibitedRoomTags + talkRequiredRoomTags
               + talkProhibitedRoomTags;
    }
    std::int64_t softPenaltyTotal() const
    {
        return 10
                   * (themeTrackConflict + themeTrackRoomStability
                      + sectorConflict + contentAudienceLevelFlowViolation
                      + sameDayTalks + popularTalks)
               + audienceTypeThemeTrackConflict + 100 * contentConflict
               + 20
                     * (speakerPreferredTimeslotTags
                        + speakerUndesiredTimeslotTags
                        + talkPreferredTimeslotTags + talkUndesiredTimeslotTags
                        + speakerPreferredRoomTags + speakerUndesiredRoomTags
                        + talkPreferredRoomTags + talkUndesiredRoomTags
                        + speakerMakespan);
    }
    std::int64_t softRewardTotal() const
    {
        return audienceTypeDiversity + audienceLevelDiversity
               + 10 * languageDiversity;
    }
    Score toScore() const
    {
        return Score{
            -hardTotal(),
            static_cast<double>(softRewardTotal() - softPenaltyTotal())};
    }
};

// Port of the Timefold conference-scheduling
// ConferenceSchedulingConstraintProvider (16 hard + 20 soft constraints),
// plus one synthetic hard constraint replacing the quickstart's per-TalkType
// value ranges.
//
// calculateScore does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under FULL_ASSERT).
//
// Hard (weight x units; units always scale with timeslot minutes):
//   h0. talkTypeCompatibility        100000 x max(duration, 1) per talk whose
//                                    timeslot or room does not admit its talk
//                                    type. SYNTHETIC: the Java quickstart
//                                    makes such assignments impossible via
//                                    @ValueRangeProvider (talkType-compatible
//                                    timeslots/rooms); the engine's single
//                                    global value range cannot, so the port
//                                    forbids them by penalty instead. Never
//                                    fires on a Java-feasible solution.
//   h1. roomUnavailableTimeslot      100000 x duration
//   h2. roomConflict                 1000 x overlap per same-room pair
//   h3. speakerUnavailableTimeslot   100 x duration per unavailable speaker
//   h4. speakerConflict              10 x overlap per shared speaker of an
//                                    overlapping pair
//   h5. talkPrerequisiteTalks        10 x combined duration per (a, b) with
//                                    a in b.prerequisites, end(a) > start(b)
//   h6. talkMutuallyExclusiveTalksTags  1 x sharedTags x overlap
//   h7. consecutiveTalksPause        1 x combined duration per pair sharing a
//                                    speaker without the minimum pause
//   h8. crowdControl                 1 x duration per risk>0 talk sharing its
//                                    timeslot with >= 2 other risk>0 talks
//   h9-h16. required/prohibited timeslot/room tags (talk and speaker)
//                                    1 x tagCount x duration
// Soft:
//   s1. themeTrackConflict           -10 x themes x overlap
//   s2. themeTrackRoomStability      -10 x themes x combined (same day,
//                                    different rooms)
//   s3. sectorConflict               -10 x sectors x overlap
//   s4. audienceTypeDiversity        +1 x audiences x slot duration (same
//                                    timeslot)
//   s5. audienceTypeThemeTrackConflict  -1 x themes x audiences x overlap
//   s6. audienceLevelDiversity       +1 x slot duration (same timeslot,
//                                    different level)
//   s7. contentAudienceLevelFlowViolation  -10 x content x combined per
//                                    (a, b) with level(a) < level(b) and
//                                    end(a) > start(b)
//   s8. contentConflict              -100 x content x overlap
//   s9. languageDiversity            +10 x slot duration (same timeslot,
//                                    different language)
//   s10. sameDayTalks                -10 x (content + themes) x combined per
//                                    pair sharing content/theme on different
//                                    days
//   s11. popularTalks                -10 x combined per (a, b) with
//                                    favorites(a) < favorites(b) and
//                                    capacity(a) > capacity(b)
//   s12-s19. preferred/undesired timeslot/room tags (talk and speaker)
//                                    -20 x tagCount x duration
//   s20. speakerMakespan             -20 x (daySpan - 1) x 8 x 60 per speaker
//                                    whose talks span more than 2 days
// Unassigned talks (slotRoom == -1) contribute nothing: the construction
// heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    static Score calculateScore(ConferenceSchedule &schedule);
    static Breakdown computeBreakdown(ConferenceSchedule &schedule);

    // The same full recompute on an ALREADY prepared schedule. The plugin
    // components (cs/constraints.hpp) need it: the engine hands them a const
    // solution, and re-running prepare() there would be both illegal and
    // pointless. Undefined unless ScoreCalculator::prepare has run.
    static Breakdown breakdownOfPrepared(const ConferenceSchedule &schedule);

    // Interns every tag/language into dense ids (sorted id vectors per
    // object), flattens the compatibility/unavailability tables and the
    // per-timeslot day/duration columns. Idempotent (guarded by
    // ConferenceSchedule::prepared). Must run before scoring;
    // calculateScore/solve call it.
    static void prepare(ConferenceSchedule &schedule);

    // --- shared building blocks (sorted interned-id vectors) ---
    static int overlappingCount(const std::vector<int> &left,
                                const std::vector<int> &right);
    static int missingCount(const std::vector<int> &required,
                            const std::vector<int> &available);

    static std::int64_t
    overlapMinutes(const ConferenceSchedule &s, int slotA, int slotB)
    {
        const std::int64_t lo
            = std::max(s.slotStart[slotA], s.slotStart[slotB]);
        const std::int64_t hi = std::min(s.slotEnd[slotA], s.slotEnd[slotB]);
        return hi > lo ? hi - lo : 0;
    }

    // Timeslot::pauseExists of the quickstart: false when the slots overlap
    // (same slot included), true on different days, otherwise the same-day
    // gap must reach pauseInMinutes.
    static bool pauseExists(const ConferenceSchedule &s,
                            int slotA,
                            int slotB,
                            int pauseInMinutes)
    {
        if (slotA == slotB || overlapMinutes(s, slotA, slotB) > 0)
            return false;
        if (s.slotDay[slotA] != s.slotDay[slotB])
            return true;
        const std::int64_t gap = s.slotStart[slotA] >= s.slotEnd[slotB]
                                     ? s.slotStart[slotA] - s.slotEnd[slotB]
                                     : s.slotStart[slotB] - s.slotEnd[slotA];
        return gap >= pauseInMinutes;
    }

    static bool
    typeAllowsSlot(const ConferenceSchedule &s, int talkType, int slot)
    {
        return s.typeSlotAllowed[static_cast<std::size_t>(talkType)
                                     * s.timeslotCount()
                                 + slot]
               != 0;
    }
    static bool
    typeAllowsRoom(const ConferenceSchedule &s, int talkType, int room)
    {
        return s.typeRoomAllowed[static_cast<std::size_t>(talkType)
                                     * s.roomCount()
                                 + room]
               != 0;
    }
    static bool
    roomUnavailableAt(const ConferenceSchedule &s, int room, int slot)
    {
        return s.roomUnavailableFlat[static_cast<std::size_t>(room)
                                         * s.timeslotCount()
                                     + slot]
               != 0;
    }
    static bool
    speakerUnavailableAt(const ConferenceSchedule &s, int speaker, int slot)
    {
        return s.speakerUnavailableFlat[static_cast<std::size_t>(speaker)
                                            * s.timeslotCount()
                                        + slot]
               != 0;
    }
};

}  // namespace cs

#endif  // CS_SCORE_CALCULATOR_HPP

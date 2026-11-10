#ifndef CS_SCORE_CALCULATOR_HPP
#define CS_SCORE_CALCULATOR_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cs/domain.hpp"

namespace cs
{

// Per-constraint match-unit totals of one full recompute. Each member is named
// after its constraint's registry name (cs/constraints/<subject>_cost.hpp) and
// holds the sum of the constraint's match units over all matches, all
// counted positive (the reward members too; their components negate them).
// The constraint weights are applied only in hardTotal() / softPenaltyTotal()
// / softRewardTotal(), which read each component's kDefaultWeight
// (score_calculator.cpp).
struct Breakdown
{
    // Hard units.
    std::int64_t talkTypeIncompatibilityCost = 0;  // see h0 below
    std::int64_t roomUnavailabilityCost = 0;       // duration
    std::int64_t roomConflictCost = 0;             // overlap minutes
    std::int64_t speakerUnavailabilityCost = 0;    // duration per speaker
    std::int64_t speakerConflictCost = 0;  // overlap minutes per shared speaker
    std::int64_t talkPrerequisiteTalksCost = 0;           // combined duration
    std::int64_t talkMutuallyExclusiveTalksTagsCost = 0;  // tags x overlap
    std::int64_t consecutiveTalksPauseCost = 0;           // combined duration
    std::int64_t crowdControlRiskCost = 0;                // duration
    std::int64_t speakerRequiredTimeslotTagsCost = 0;     // missing x duration
    std::int64_t speakerProhibitedTimeslotTagsCost = 0;  // prevailing x minutes
    std::int64_t talkRequiredTimeslotTagsCost = 0;
    std::int64_t talkProhibitedTimeslotTagsCost = 0;
    std::int64_t speakerRequiredRoomTagsCost = 0;
    std::int64_t speakerProhibitedRoomTagsCost = 0;
    std::int64_t talkRequiredRoomTagsCost = 0;
    std::int64_t talkProhibitedRoomTagsCost = 0;

    // Soft penalty units.
    std::int64_t themeTrackConflictCost = 0;    // themes x overlap
    std::int64_t themeTrackRoomChangeCost = 0;  // themes x combined duration
    std::int64_t sectorConflictCost = 0;        // sectors x overlap
    std::int64_t audienceTypeThemeTrackConflictCost = 0;  // th x aud x overlap
    std::int64_t contentAudienceLevelFlowViolationCost = 0;  // content x comb.
    std::int64_t contentConflictCost = 0;       // content x overlap
    std::int64_t differentDayTalksCost = 0;     // (themes + content) x combined
    std::int64_t popularTalksRoomSizeCost = 0;  // combined duration
    std::int64_t speakerPreferredTimeslotTagsCost = 0;  // missing x duration
    std::int64_t speakerUndesiredTimeslotTagsCost = 0;  // prevailing x duration
    std::int64_t talkPreferredTimeslotTagsCost = 0;
    std::int64_t talkUndesiredTimeslotTagsCost = 0;
    std::int64_t speakerPreferredRoomTagsCost = 0;
    std::int64_t speakerUndesiredRoomTagsCost = 0;
    std::int64_t talkPreferredRoomTagsCost = 0;
    std::int64_t talkUndesiredRoomTagsCost = 0;
    std::int64_t speakerMakespanCost = 0;  // (day span - 1) x 8h per speaker

    // Soft reward units (positive here; the *DiversityCost components report
    // them negated, as rewards).
    std::int64_t audienceTypeDiversityCost = 0;   // audiences x slot duration
    std::int64_t audienceLevelDiversityCost = 0;  // slot duration
    std::int64_t languageDiversityCost = 0;       // slot duration

    // Weighted totals at the components' kDefaultWeight (all integral, so the
    // sums are exact in int64).
    std::int64_t hardTotal() const;
    std::int64_t softPenaltyTotal() const;
    std::int64_t softRewardTotal() const;
    Score toScore() const
    {
        return Score{
            -hardTotal(),
            static_cast<double>(softRewardTotal() - softPenaltyTotal())};
    }
};

// The conference-scheduling score as one monolithic function: 17 hard +
// 20 soft constraints.
//
// score does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under ScoreChecks::Full).
//
// Hard (default weight x units; units always scale with timeslot minutes;
// the weights are the components' kDefaultWeight):
//   h0. talkTypeIncompatibilityCost
//           100000 x max(duration, 1) per talk whose timeslot or room does
//           not admit its talk type. The engine's single global
//           value range cannot restrict a talk to its type-compatible
//           timeslots/rooms, so such assignments are forbidden by penalty.
//   h1. roomUnavailabilityCost         100000 x duration
//   h2. roomConflictCost               1000 x overlap per same-room pair
//   h3. speakerUnavailabilityCost      100 x duration per unavailable speaker
//   h4. speakerConflictCost            10 x overlap per shared speaker of an
//                                      overlapping pair
//   h5. talkPrerequisiteTalksCost      10 x combined duration per (a, b) with
//                                      a in b.prerequisites, end(a) > start(b)
//   h6. talkMutuallyExclusiveTalksTagsCost
//                                      1 x sharedTags x overlap
//   h7. consecutiveTalksPauseCost      1 x combined duration per pair sharing
//                                      a speaker without the minimum pause
//   h8. crowdControlRiskCost           1 x duration per risk>0 talk sharing
//                                      its timeslot with >= 2 other risk>0
//                                      talks
//   h9-h16. required/prohibited timeslot/room tags (talk and speaker)
//                                      1 x tagCount x duration
// Soft:
//   s1. themeTrackConflictCost         -10 x themes x overlap
//   s2. themeTrackRoomChangeCost       -10 x themes x combined (same day,
//                                      different rooms)
//   s3. sectorConflictCost             -10 x sectors x overlap
//   s4. audienceTypeDiversityCost      +1 x audiences x slot duration (same
//                                      timeslot; a reward)
//   s5. audienceTypeThemeTrackConflictCost
//                                      -1 x themes x audiences x overlap
//   s6. audienceLevelDiversityCost     +1 x slot duration (same timeslot,
//                                      different level; a reward)
//   s7. contentAudienceLevelFlowViolationCost
//                                      -10 x content x combined per (a, b)
//                                      with level(a) < level(b) and
//                                      end(a) > start(b)
//   s8. contentConflictCost            -100 x content x overlap
//   s9. languageDiversityCost          +10 x slot duration (same timeslot,
//                                      different language; a reward)
//   s10. differentDayTalksCost         -10 x (content + themes) x combined
//                                      per pair sharing content/theme on
//                                      different days
//   s11. popularTalksRoomSizeCost      -10 x combined per (a, b) with
//                                      favorites(a) < favorites(b) and
//                                      capacity(a) > capacity(b)
//   s12-s19. preferred/undesired timeslot/room tags (talk and speaker)
//                                      -20 x tagCount x duration
//   s20. speakerMakespanCost           -20 x (daySpan - 1) x 8 x 60 per
//                                      speaker whose talks span more than 2
//                                      days
// Unassigned talks (slotRoom == -1) contribute nothing: the construction
// heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    static Score score(ConferenceSchedule &schedule);
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
    // score/solve call it.
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

    // Whether a pause exists between two slots: false when the slots overlap
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

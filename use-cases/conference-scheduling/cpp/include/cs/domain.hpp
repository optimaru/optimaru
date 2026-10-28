#ifndef CS_DOMAIN_HPP
#define CS_DOMAIN_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace cs
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The Java quickstart already uses HardSoftScore, so the mapping onto the
// engine's HardSoftScore(int64 hard, double soft) is the identity: hard is
// the weighted violation-minute sum (0 == feasible), soft is the (always
// integral) weighted preference/diversity balance stored in the engine's
// double field. No level remapping needed.
using Score = optimaru::HardSoftScore;

// ---------------------------------------------------------------------------
// Timeslot  (problem fact, half of the planning value)
// ---------------------------------------------------------------------------
// The quickstart's Timeslot (LocalDateTime start/end + talk types + tags),
// with the date-times flattened to minutes since an arbitrary common origin
// whose day boundaries fall on multiples of 1440 (e.g. minutes since the
// conference's first midnight). Day-of assertions ("same day", makespan day
// spans) are startMinute / 1440, exactly LocalDateTime::toLocalDate().
struct Timeslot
{
    std::string id;
    std::int64_t startMinute = 0;
    std::int64_t endMinute = 0;
    // Indices into ConferenceSchedule::talkTypes: which talk types this slot
    // may host (the quickstart's per-TalkType compatible-timeslot value
    // range; see the talkTypeCompatibility note on ScoreCalculator).
    std::vector<int> talkTypes;
    std::vector<std::string> tags;

    int durationInMinutes() const
    {
        return static_cast<int>(endMinute - startMinute);
    }
};

// ---------------------------------------------------------------------------
// Room  (problem fact, the other half of the planning value)
// ---------------------------------------------------------------------------
struct Room
{
    std::string id;
    std::string name;
    int capacity = 0;
    // Indices into ConferenceSchedule::talkTypes (compatible-room range).
    std::vector<int> talkTypes;
    // Timeslot indices where the room cannot be used (hard).
    std::vector<int> unavailableTimeslots;
    std::vector<std::string> tags;
};

// ---------------------------------------------------------------------------
// Speaker  (problem fact)
// ---------------------------------------------------------------------------
struct Speaker
{
    std::string id;
    std::string name;
    // Timeslot indices where the speaker is unavailable (hard).
    std::vector<int> unavailableTimeslots;
    std::vector<std::string> requiredTimeslotTags;
    std::vector<std::string> preferredTimeslotTags;
    std::vector<std::string> prohibitedTimeslotTags;
    std::vector<std::string> undesiredTimeslotTags;
    std::vector<std::string> requiredRoomTags;
    std::vector<std::string> preferredRoomTags;
    std::vector<std::string> prohibitedRoomTags;
    std::vector<std::string> undesiredRoomTags;
};

// ---------------------------------------------------------------------------
// Talk  (@PlanningEntity)
// ---------------------------------------------------------------------------
// slotRoom is the only planning variable. The engine supports one BASIC int
// variable per entity, so the quickstart's two variables (timeslot, room)
// are composed into a single value (the school-timetabling encoding):
//   slotRoom = timeslotIndex * roomCount + roomIndex
// -1 means unassigned (both quickstart variables null). Decoding helpers
// live on ConferenceSchedule, which knows roomCount.
struct Talk
{
    std::string code;
    std::string title;
    int talkType = 0;           // index into ConferenceSchedule::talkTypes
    std::vector<int> speakers;  // indices into ConferenceSchedule::speakers
    std::vector<std::string> themeTrackTags;
    std::vector<std::string> sectorTags;
    std::vector<std::string> audienceTypes;
    int audienceLevel = 0;
    std::vector<std::string> contentTags;
    std::string language;
    std::vector<std::string> requiredTimeslotTags;
    std::vector<std::string> preferredTimeslotTags;
    std::vector<std::string> prohibitedTimeslotTags;
    std::vector<std::string> undesiredTimeslotTags;
    std::vector<std::string> requiredRoomTags;
    std::vector<std::string> preferredRoomTags;
    std::vector<std::string> prohibitedRoomTags;
    std::vector<std::string> undesiredRoomTags;
    std::vector<std::string> mutuallyExclusiveTalksTags;
    std::vector<int> prerequisiteTalks;  // indices into schedule.talks
    int favoriteCount = 0;
    int crowdControlRisk = 0;

    int slotRoom = -1;  // planning variable; -1 == unassigned

    // Extension over the quickstart (which has no @PlanningPin): pinned
    // talks keep their slotRoom fixed — scored like any talk but never
    // moved/swapped/ruined by the solver.
    bool pinned = false;
};

// Interned (sorted, deduplicated) tag-id mirrors of the eight preference tag
// sets shared by Talk and Speaker. Filled by ScoreCalculator::prepare.
struct PreferenceTagIds
{
    std::vector<int> requiredTimeslot;
    std::vector<int> preferredTimeslot;
    std::vector<int> prohibitedTimeslot;
    std::vector<int> undesiredTimeslot;
    std::vector<int> requiredRoom;
    std::vector<int> preferredRoom;
    std::vector<int> prohibitedRoom;
    std::vector<int> undesiredRoom;
};

// ---------------------------------------------------------------------------
// ConferenceSchedule  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct ConferenceSchedule
{
    std::string name;
    std::vector<std::string> talkTypes;  // e.g. {"Breakout", "Lab"}
    std::vector<Timeslot> timeslots;
    std::vector<Room> rooms;
    std::vector<Speaker> speakers;
    std::vector<Talk> talks;

    // The quickstart's ConferenceConstraintProperties problem fact.
    int minimumConsecutiveTalksPauseInMinutes = 30;

    Score score;

    // Set once by ScoreCalculator::prepare; guards against re-interning the
    // tag/day tables on every calculateScore call.
    bool prepared = false;

    // --- filled by ScoreCalculator::prepare -------------------------------
    // Flat per-timeslot copies (indexable without touching the structs):
    std::vector<std::int64_t> slotStart;
    std::vector<std::int64_t> slotEnd;
    std::vector<int> slotDur;
    std::vector<std::int64_t> slotDay;     // calendar day = startMinute / 1440
    std::vector<int> slotDayIndex;         // dense 0..dayCount-1
    std::vector<std::int64_t> daysSorted;  // dense day index -> calendar day
    int dayCount = 0;
    // Compatibility tables: talkType t may use slot s / room r.
    std::vector<char> typeSlotAllowed;  // t * timeslotCount + s
    std::vector<char> typeRoomAllowed;  // t * roomCount + r
    // Availability tables (1 == unavailable):
    std::vector<char> roomUnavailableFlat;     // r * timeslotCount + s
    std::vector<char> speakerUnavailableFlat;  // k * timeslotCount + s
    // Interned sorted tag ids (one shared string pool; comparisons always
    // stay within one tag family, so sharing the pool cannot collide):
    std::vector<std::vector<int>> slotTagIds;    // per timeslot
    std::vector<std::vector<int>> roomTagIds;    // per room
    std::vector<std::vector<int>> talkThemeIds;  // per talk
    std::vector<std::vector<int>> talkSectorIds;
    std::vector<std::vector<int>> talkAudienceIds;
    std::vector<std::vector<int>> talkContentIds;
    std::vector<std::vector<int>> talkMutexIds;
    std::vector<std::vector<int>> talkPrereqSorted;    // sorted talk indices
    std::vector<std::vector<int>> talkSpeakersSorted;  // sorted speaker ids
    std::vector<int> talkLanguageId;                   // interned language
    std::vector<PreferenceTagIds> talkPrefIds;         // per talk
    std::vector<PreferenceTagIds> speakerPrefIds;      // per speaker

    // --- composite slotRoom encoding helpers ------------------------------
    int timeslotCount() const { return static_cast<int>(timeslots.size()); }
    int roomCount() const { return static_cast<int>(rooms.size()); }
    int talkCount() const { return static_cast<int>(talks.size()); }
    int slotRoomCount() const { return timeslotCount() * roomCount(); }
    int slotRoomOf(int timeslotIndex, int roomIndex) const
    {
        return timeslotIndex * roomCount() + roomIndex;
    }
    int timeslotIndexOf(int slotRoom) const { return slotRoom / roomCount(); }
    int roomIndexOf(int slotRoom) const { return slotRoom % roomCount(); }
};

}  // namespace cs

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization  (the @PlanningEntity/@PlanningVariable
// annotations of the C++ engine): entities are talks, the planning variable
// is Talk::slotRoom, the value range is the (timeslot x room) grid.
//
// NOTE on value ranges: the Java quickstart restricts each Talk's value
// ranges to its TalkType's compatible timeslots/rooms (@ValueRangeProvider on
// the entity). The engine's BASIC variable has one global range, so the port
// enforces compatibility with the synthetic hard constraint
// talkTypeCompatibility instead (see cs/score_calculator.hpp) and biases the
// search toward compatible values (see cs/solver.hpp).
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<cs::ConferenceSchedule>
{
    static constexpr optimaru::VariableKind kind
        = optimaru::VariableKind::BASIC;
    static int entityCount(const cs::ConferenceSchedule &s)
    {
        return static_cast<int>(s.talks.size());
    }
    static int valueCount(const cs::ConferenceSchedule &s)
    {
        return s.slotRoomCount();
    }
    static int getVariable(const cs::ConferenceSchedule &s, int talkIndex)
    {
        return s.talks[talkIndex].slotRoom;
    }
    static void
    setVariable(cs::ConferenceSchedule &s, int talkIndex, int slotRoom)
    {
        s.talks[talkIndex].slotRoom = slotRoom;
    }
    static bool isPinned(const cs::ConferenceSchedule &s, int talkIndex)
    {
        return s.talks[talkIndex].pinned;
    }
};

#endif  // CS_DOMAIN_HPP

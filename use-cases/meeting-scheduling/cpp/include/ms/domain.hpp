#ifndef MS_DOMAIN_HPP
#define MS_DOMAIN_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace ms
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The Java quickstart scores with HardMediumSoftScore; the engine provides
// HardSoftScore(int64 hard, double soft). Mapping rule (also in the README):
//
//   hard = Java hard, unchanged (violation counts / grains; 0 == feasible)
//   soft = -(javaMediumPenalty * kMediumScale + javaSoftPenalty)
//
// i.e. the medium level (required+preferred / preferred attendance conflicts)
// is folded into the engine's soft as an offset of kMediumScale = 1e6 per
// medium unit, keeping the level priority: for realistic instances the raw
// soft penalty (grain indices, break/overlap counts, room capacities) stays
// far below 1e6, so medium always dominates soft. Both penalty sums are
// integers, so the double representation is exact up to 2^53.
using Score = optimaru::HardSoftScore;

// The medium -> soft folding offset (see the Score comment above).
constexpr double kMediumScale = 1e6;

// Java TimeGrain.GRAIN_LENGTH_IN_MINUTES.
constexpr int kGrainLengthInMinutes = 15;

// ---------------------------------------------------------------------------
// Person  (problem fact)
// ---------------------------------------------------------------------------
// Attendances reference people by index into MeetingSchedule::people.
struct Person
{
    std::string id;
    std::string fullName;
};

// ---------------------------------------------------------------------------
// TimeGrain  (problem fact, value range half of the composite variable)
// ---------------------------------------------------------------------------
// A 15-minute slot. grainIndex is the Java grainIndex: the unit in which
// durations, overlaps and "as soon as possible" penalties are measured (the
// demo data numbers grains 1..160). dayOfYear + startingMinuteOfDay define
// the calendar position ("start and end on same day", room stability order).
struct TimeGrain
{
    std::string id;
    int grainIndex = 0;
    int dayOfYear = 0;
    int startingMinuteOfDay = 0;
};

// ---------------------------------------------------------------------------
// Room  (problem fact, the other half of the composite variable)
// ---------------------------------------------------------------------------
struct Room
{
    std::string id;
    std::string name;
    int capacity = 0;
};

// ---------------------------------------------------------------------------
// Meeting  (problem fact)
// ---------------------------------------------------------------------------
// requiredAttendees/preferredAttendees are the Java RequiredAttendance /
// PreferredAttendance facts, stored as person indices (one entry == one
// attendance fact). requiredCapacity == required + preferred count, exactly
// like Java's Meeting::getRequiredCapacity.
struct Meeting
{
    std::string id;
    std::string topic;
    int durationInGrains = 1;
    std::vector<int> requiredAttendees;
    std::vector<int> preferredAttendees;
    bool entireGroupMeeting = false;  // informational, like the Java field

    int requiredCapacity() const
    {
        return static_cast<int>(requiredAttendees.size()
                                + preferredAttendees.size());
    }
};

// ---------------------------------------------------------------------------
// MeetingAssignment  (@PlanningEntity)
// ---------------------------------------------------------------------------
// The Java entity has two planning variables (startingTimeGrain, room). The
// engine supports one BASIC int variable per entity, so they are composed
// into a single value (the school-timetabling slotRoom pattern):
//   grainRoom = timeGrainPos * roomCount + roomIndex
// where timeGrainPos indexes MeetingSchedule::timeGrains. -1 means unassigned
// (both Java variables null). A meeting occupies durationInGrains consecutive
// grain indices starting at its grain's grainIndex; every overlap constraint
// measures that full interval, not just the starting grain.
struct MeetingAssignment
{
    std::string id;
    int meeting = 0;  // index into MeetingSchedule::meetings (1:1, see below)

    int grainRoom = -1;  // planning variable; -1 == unassigned

    // Pinned assignments keep their grainRoom fixed (@PlanningPin): scored
    // like any assignment but never selected for a move.
    bool pinned = false;
};

// ---------------------------------------------------------------------------
// prepare() products (see ScoreCalculator::prepare)
// ---------------------------------------------------------------------------
// One meeting pair that shares at least one attendee, with the pairwise
// attendance-multiplicity weights used by the attendance constraints.
// meeting1 < meeting2; the "forward" direction is meeting1 -> meeting2.
struct ConflictArc
{
    int meeting1 = 0;
    int meeting2 = 0;
    std::int64_t requiredWeight = 0;    // shared required-attendance pairs
    std::int64_t preferredWeight = 0;   // shared preferred-attendance pairs
    std::int64_t reqPrefForward = 0;    // required in m1, preferred in m2
    std::int64_t reqPrefReverse = 0;    // required in m2, preferred in m1
    std::int64_t attendanceWeight = 0;  // any-attendance pairs (room stability)
};

// ---------------------------------------------------------------------------
// MeetingSchedule  (@PlanningSolution)
// ---------------------------------------------------------------------------
// Model note: meetings and meeting assignments are 1:1, like the quickstart's
// demo data model (every Meeting has exactly one MeetingAssignment); prepare()
// rejects duplicates. Attendances are problem facts derived from the meetings.
struct MeetingSchedule
{
    std::vector<Person> people;
    std::vector<TimeGrain> timeGrains;
    std::vector<Room> rooms;
    std::vector<Meeting> meetings;
    std::vector<MeetingAssignment> meetingAssignments;

    Score score;

    // Set once by ScoreCalculator::prepare; guards against rebuilding the
    // derived tables on every calculateScore call.
    bool prepared = false;

    // Filled by ScoreCalculator::prepare:
    int grainIndexBase = 0;       // min grainIndex over timeGrains
    int maxDurationInGrains = 1;  // max over meetings (>= 1)
    // Occupancy span in grain-index offsets (grainIndex - grainIndexBase):
    // (maxGrainIndex - minGrainIndex + 1) + maxDurationInGrains, so the last
    // occupied offset of any assignment fits even when it runs past the last
    // existing grain (overtime).
    int occupancySpan = 0;
    // dayOfYear of every TimeGrain fact at a grain-index offset (normally one
    // entry; empty == no grain exists at that index -> overtime when a
    // meeting ends there).
    std::vector<std::vector<int>> grainDaysAtOffset;
    // Per room r: sum over rooms R with capacity(R) > capacity(r) of
    // (capacity(R) - capacity(r)) — the "assign larger rooms first" penalty
    // of one assignment in room r.
    std::vector<std::int64_t> largerRoomPenalty;
    std::vector<int> requiredCapacityOf;   // per meeting
    std::vector<int> assignmentOfMeeting;  // meeting -> assignment index (1:1)
    // Meeting pairs sharing attendees (m1 < m2), for the full recompute.
    std::vector<ConflictArc> conflictArcs;
    // Adjacency view of conflictArcs' attendanceWeight for the incremental
    // room-stability delta: per meeting, (other meeting, weight).
    std::vector<std::vector<std::pair<int, std::int64_t>>> stabilityNeighbors;
    // Same-meeting attendance-pair weights (nonzero only when one person
    // holds several attendance facts of one meeting; Java joins them too).
    std::vector<std::int64_t> selfRequiredPairs;      // per meeting
    std::vector<std::int64_t> selfPreferredPairs;     // per meeting
    std::vector<std::int64_t> selfRequiredPreferred;  // per meeting

    // --- composite grainRoom encoding helpers ---
    int timeGrainCount() const { return static_cast<int>(timeGrains.size()); }
    int roomCount() const { return static_cast<int>(rooms.size()); }
    int grainRoomCount() const { return timeGrainCount() * roomCount(); }
    int grainRoomOf(int timeGrainPos, int roomIndex) const
    {
        return timeGrainPos * roomCount() + roomIndex;
    }
    int timeGrainPosOf(int grainRoom) const { return grainRoom / roomCount(); }
    int roomIndexOf(int grainRoom) const { return grainRoom % roomCount(); }

    // --- prepared-table lookups ---
    int grainCountAtOffset(int offset) const
    {
        return static_cast<int>(grainDaysAtOffset[offset].size());
    }
    int grainCountAtOffsetOnDay(int offset, int dayOfYear) const
    {
        int count = 0;
        for (int day : grainDaysAtOffset[offset])
        {
            if (day == dayOfYear)
                ++count;
        }
        return count;
    }
};

}  // namespace ms

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization  (the @PlanningEntity/@PlanningVariable
// annotations of the C++ engine): entities are the meeting assignments, the
// planning variable is MeetingAssignment::grainRoom, the value range is the
// (timeGrain x room) grid — the composite of the Java entity's two
// @PlanningVariable value ranges.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<ms::MeetingSchedule>
{
    static constexpr optimaru::VariableKind kind
        = optimaru::VariableKind::BASIC;
    static int entityCount(const ms::MeetingSchedule &s)
    {
        return static_cast<int>(s.meetingAssignments.size());
    }
    static int valueCount(const ms::MeetingSchedule &s)
    {
        return s.grainRoomCount();
    }
    static int getVariable(const ms::MeetingSchedule &s, int assignmentIndex)
    {
        return s.meetingAssignments[assignmentIndex].grainRoom;
    }
    static void
    setVariable(ms::MeetingSchedule &s, int assignmentIndex, int grainRoom)
    {
        s.meetingAssignments[assignmentIndex].grainRoom = grainRoom;
    }
    static bool isPinned(const ms::MeetingSchedule &s, int assignmentIndex)
    {
        return s.meetingAssignments[assignmentIndex].pinned;
    }
};

#endif  // MS_DOMAIN_HPP

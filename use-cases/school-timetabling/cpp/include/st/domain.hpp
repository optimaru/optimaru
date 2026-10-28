#ifndef ST_DOMAIN_HPP
#define ST_DOMAIN_HPP

#include <map>
#include <set>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace st
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The score type comes from the generic engine (include/optimaru/). All seven school
// timetabling constraints count whole lesson pairs / lessons, so both levels
// stay integral (soft is stored in the engine's double field).
using Score = optimaru::HardSoftScore;

// ---------------------------------------------------------------------------
// Timeslot  (problem fact)
// ---------------------------------------------------------------------------
// The quickstart's Timeslot (dayOfWeek + LocalTime start/end), remodeled as a
// (day, period) pair: Korean schools schedule by numbered periods (교시), not
// clock times. day 0 = Monday; period is 1-based. Two periods are consecutive
// when they share a day and differ by exactly 1 — the quickstart's
// getTime()-arithmetic in TimeTableConstraintProvider reduces to that.
// (day, period) pairs are assumed unique within a Timetable.
struct Timeslot
{
    int day = 0;     // 0=Mon .. 4=Fri (5=Sat allowed)
    int period = 1;  // 1-based period-of-day (교시)
};

// ---------------------------------------------------------------------------
// Room  (problem fact)
// ---------------------------------------------------------------------------
// `type` is informational only (일반/과학실/체육관/...): like the quickstart,
// no hard constraint restricts which lessons a room may host.
struct Room
{
    std::string name;
    std::string type;
};

// ---------------------------------------------------------------------------
// Lesson  (@PlanningEntity)
// ---------------------------------------------------------------------------
// slotRoom is the only planning variable. The engine supports one BASIC int
// variable per entity, so the quickstart's two variables (timeslot, room) are
// composed into a single value:
//   slotRoom = timeslotIndex * roomCount + roomIndex
// -1 means unassigned (both quickstart variables null). Decoding helpers live
// on Timetable, which knows roomCount.
struct Lesson
{
    std::string id;
    std::string subject;
    std::string teacher;
    std::string studentGroup;

    int slotRoom = -1;  // planning variable; -1 == unassigned

    // Pinned lessons keep their slotRoom fixed (e.g. a lesson nailed to a
    // specific period by school policy). They are scored like any lesson but
    // never moved/reassigned by the solver.
    bool pinned = false;

    // Precomputed by ScoreCalculator::prepare (interned string ids; kept at
    // the end so the 4-field brace-initialisers elsewhere still compile):
    int teacherId = -1;
    int groupId = -1;
    int groupSubjectId = -1;  // (studentGroup, subject) pair id
};

// ---------------------------------------------------------------------------
// Timetable  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct Timetable
{
    std::vector<Timeslot> timeslots;
    std::vector<Room> rooms;
    std::vector<Lesson> lessons;

    // Teacher unavailability (extension over the quickstart): the timeslot
    // indices where a teacher cannot teach, keyed by teacher name. Teachers
    // absent from the map are always available; entries for teachers without
    // lessons are ignored.
    std::map<std::string, std::set<int>> teacherUnavailable;

    Score score;

    // Set once by ScoreCalculator::prepare; guards against re-interning the
    // teacher/group ids on every calculateScore call.
    bool prepared = false;

    // Filled by ScoreCalculator::prepare:
    int teacherCount = 0;
    int groupCount = 0;
    int groupSubjectCount = 0;
    // Period adjacency: same-day period+1 / period-1 timeslot index, or -1.
    std::vector<int> nextSlot;
    std::vector<int> prevSlot;
    // Flattened teacherUnavailable: teacherId * timeslotCount() + timeslot.
    std::vector<char> teacherUnavailableFlat;

    // --- composite slotRoom encoding helpers ---
    int timeslotCount() const { return static_cast<int>(timeslots.size()); }
    int roomCount() const { return static_cast<int>(rooms.size()); }
    int slotRoomCount() const { return timeslotCount() * roomCount(); }
    int slotRoomOf(int timeslotIndex, int roomIndex) const
    {
        return timeslotIndex * roomCount() + roomIndex;
    }
    int timeslotIndexOf(int slotRoom) const { return slotRoom / roomCount(); }
    int roomIndexOf(int slotRoom) const { return slotRoom % roomCount(); }
};

}  // namespace st

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization  (the @PlanningEntity/@PlanningVariable
// annotations of the C++ engine): entities are lessons, the planning variable
// is Lesson::slotRoom, the value range is the (timeslot x room) grid.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<st::Timetable>
{
    static constexpr optimaru::VariableKind kind
        = optimaru::VariableKind::BASIC;
    static int entityCount(const st::Timetable &t)
    {
        return static_cast<int>(t.lessons.size());
    }
    static int valueCount(const st::Timetable &t) { return t.slotRoomCount(); }
    static int getVariable(const st::Timetable &t, int lessonIndex)
    {
        return t.lessons[lessonIndex].slotRoom;
    }
    static void setVariable(st::Timetable &t, int lessonIndex, int slotRoom)
    {
        t.lessons[lessonIndex].slotRoom = slotRoom;
    }
    static bool isPinned(const st::Timetable &t, int lessonIndex)
    {
        return t.lessons[lessonIndex].pinned;
    }
};

#endif  // ST_DOMAIN_HPP

#ifndef ST_CANONICAL_DOMAIN_HPP
#define ST_CANONICAL_DOMAIN_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

// ---------------------------------------------------------------------------
// Canonical full-parity school-timetabling domain (st::canon)
// ---------------------------------------------------------------------------
// A second, independent domain next to the quickstart port (st::Timetable):
// it mirrors the CP-SAT reference package (benchmarks/school-timetabling/cpsat/
// teacher_scheduling) constraint-for-constraint — hard H1~H10 and soft S1~S6
// with the exact semantics of teacher_scheduling/validate.py — on top of the
// generic optimaru engine.
//
// Modeling decisions (fixed by design, do not change casually):
//   - The single planning variable is the timeslot index (BASIC, -1 =
//     unassigned). Rooms are NOT a variable: same-typed special rooms are
//     interchangeable, so H6 only compares per-(roomType, slot) demand against
//     the owned count (like the CP-SAT model; concrete rooms are assigned in a
//     trivial post-processing step outside the solver).
//   - One Lesson = one weekly period of one course (a course with
//     periods_per_week = 3 becomes 3 lessons sharing one courseKey).
//   - Parallel groups (분반) do NOT exist in this domain. The conversion
//     layer merges each parallel group into ONE "merged lesson" per weekly
//     period: teachers = union of the member courses' teachers, roomTypeDemand
//     = sum of the members' demands, and the merged lesson occupies exactly
//     one class slot (the group as a whole is one occupancy unit, matching
//     Course.occupancy_key in the CP-SAT domain). H5 (same slots for all
//     members) then holds by construction. S5/S6 stay exact through
//     Lesson::preferenceUnits: one (avoided, preferred) mask pair per member
//     course, each counted independently — a merged lesson penalizes exactly
//     like the member lessons it stands for in validate.py.
//   - Team teaching is a lesson with several entries in `teachers`; each
//     teacher occupies the slot individually (H4, H9, H10, S1~S3).
//   - Fixed slots (H2) are expressed as pinned lessons with a preassigned
//     slot: the engine never moves a pinned entity, so no constraint is
//     needed.
//   - Everything that merely forbids a (lesson, slot) combination — calendar
//     holes, course forbidden_slots, teacher unavailable, class unavailable —
//     is pre-baked into the per-lesson allowedSlot table and costs 1 hard
//     point per violation (H5 here numbers the "assigned outside the allowed
//     set" constraint).
namespace st::canon
{

using Score = optimaru::HardSoftScore;

// S1~S6 weights, mirroring the CP-SAT ObjectiveWeights defaults. A weight of
// 0 removes the term. soft score = -(weighted penalty sum).
struct ObjectiveWeights
{
    int teacherGap = 20;         // S1: idle period inside a teacher's day
    int teacherWorkday = 3;      // S2: each (teacher, day) with any lesson
    int teacherFreeDay = 30;     // S3: lesson on a preferred-free day
    int classGap = 10;           // S4: idle period inside a class's day
    int avoidedPeriod = 5;       // S5: lesson on an avoided period
    int nonPreferredPeriod = 1;  // S6: lesson off the preferred periods
};

// ---------------------------------------------------------------------------
// Lesson  (@PlanningEntity)
// ---------------------------------------------------------------------------
// One weekly period of one (possibly merged) course. `slot` is the only
// planning variable: the timeslot index into Timetable::dayOfSlot /
// periodOfSlot; -1 = unassigned.
struct Lesson
{
    std::string id;

    int classGroupId = -1;  // dense class id; the lesson occupies 1 class slot
    int courseKey = -1;     // dense course id; shared by the course's lessons

    // All teachers standing in this lesson (>= 2 for team teaching, union of
    // the member courses for a merged lesson). Entries must be distinct.
    std::vector<int> teachers;

    // Special-room demand per room type id (H6). Empty = no demand; a merged
    // lesson carries the SUM of its member courses' demands.
    std::vector<int> roomTypeDemand;

    // Per-slot permission table (H5), sized slotCount (empty = every slot
    // allowed). The conversion layer pre-bakes course forbidden_slots,
    // teacher unavailable and class unavailable times into this table.
    std::vector<char> allowedSlot;

    // S5/S6 preference units: one (avoidedPeriodMask, preferredPeriodMask)
    // pair per member course of a merged lesson (bit = period - 1). Every
    // unit is counted independently per assigned period, reproducing the
    // per-member-lesson counting of validate.py's compute_penalties. Left
    // empty, prepare() derives the single default unit from the courseKey's
    // period masks (the plain, unmerged case).
    std::vector<std::pair<std::uint16_t, std::uint16_t>> preferenceUnits;

    // Block (연강) group id, -1 = none. All lessons sharing a blockGroup must
    // sit on one day in consecutive periods (H8).
    int blockGroup = -1;

    // Pinned lessons keep their preassigned slot (H2: fixed_slots); the
    // engine scores them but never moves them.
    bool pinned = false;

    int slot = -1;  // planning variable; -1 == unassigned
};

// ---------------------------------------------------------------------------
// Timetable  (@PlanningSolution)
// ---------------------------------------------------------------------------
// Slots are enumerated (day, period) pairs: dayOfSlot[s] is the 0-based day,
// periodOfSlot[s] the 1-based period (교시). Days may differ in length (e.g.
// Fri has 6 periods while Mon~Thu have 7) — simply enumerate the slots that
// exist. Periods must fit a 16-bit mask (period <= 16) and days a byte mask
// (day < 8).
struct Timetable
{
    std::vector<int> dayOfSlot;     // slot -> 0-based day
    std::vector<int> periodOfSlot;  // slot -> 1-based period
    std::vector<Lesson> lessons;

    int teacherCount = 0;  // may be left 0: prepare() derives max id + 1
    int classCount = 0;    // may be left 0: prepare() derives max id + 1

    // H6: owned special rooms per room type id.
    std::vector<int> roomCapacityByType;

    // Per-teacher limits (H9/H10), sized teacherCount by prepare() with the
    // CP-SAT defaults: maxPerDay 6, maxPerWeek -1 (unlimited), maxConsecutive
    // 4. S3: preferred free days as a per-teacher day bitmask (bit d).
    std::vector<int> teacherMaxPerDay;
    std::vector<int> teacherMaxPerWeek;
    std::vector<int> teacherMaxConsecutive;
    std::vector<std::uint8_t> teacherPreferredFreeDays;

    // Per-courseKey data, sized courseCount by prepare(): the daily cap (H7,
    // default 1; a block course needs at least its block size) and the
    // subject period masks (S5/S6, bit period-1; preferred mask 0 = no
    // preference).
    std::vector<int> courseDailyLimit;
    std::vector<std::uint16_t> courseAvoidedPeriodMask;
    std::vector<std::uint16_t> coursePreferredPeriodMask;

    ObjectiveWeights weights;

    Score score;

    // Set once by ScoreCalculator::prepare (idempotent normalization +
    // derived tables below).
    bool prepared = false;

    // Filled by prepare():
    int dayCount = 0;
    int maxPeriod = 0;
    int courseCount = 0;
    int roomTypeCount = 0;
    int blockGroupCount = 0;
    // Lesson indices per block group (H8 recomputes a whole group at once).
    std::vector<std::vector<int>> blockMembers;
    // day * maxPeriod + (period - 1) -> slot index, or -1 when that (day,
    // period) does not exist. Convenience for builders/tests.
    std::vector<int> slotOfDayPeriod;

    int slotCount() const { return static_cast<int>(dayOfSlot.size()); }
    int slotAt(int day, int period) const
    {
        return slotOfDayPeriod[static_cast<std::size_t>(day) * maxPeriod
                               + period - 1];
    }
    static bool slotAllowed(const Lesson &lesson, int slot)
    {
        return lesson.allowedSlot.empty() || lesson.allowedSlot[slot] != 0;
    }
};

}  // namespace st::canon

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization: entities are lessons, the planning
// variable is Lesson::slot, the value range is the timeslot list. Pinned
// lessons (H2 fixed slots) are excluded from every move.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<st::canon::Timetable>
{
    static constexpr optimaru::VariableKind kind
        = optimaru::VariableKind::BASIC;
    static int entityCount(const st::canon::Timetable &t)
    {
        return static_cast<int>(t.lessons.size());
    }
    static int valueCount(const st::canon::Timetable &t)
    {
        return t.slotCount();
    }
    static int getVariable(const st::canon::Timetable &t, int lessonIndex)
    {
        return t.lessons[lessonIndex].slot;
    }
    static void setVariable(st::canon::Timetable &t, int lessonIndex, int slot)
    {
        t.lessons[lessonIndex].slot = slot;
    }
    static bool isPinned(const st::canon::Timetable &t, int lessonIndex)
    {
        return t.lessons[lessonIndex].pinned;
    }
};

#endif  // ST_CANONICAL_DOMAIN_HPP

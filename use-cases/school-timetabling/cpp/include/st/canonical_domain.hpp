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
// teacher_scheduling) constraint-for-constraint — the same hard and soft
// constraints under the same names (unassigned_lessons_cost ...
// non_preferred_period_cost) with the exact semantics of
// teacher_scheduling/validate.py — on top of the generic optimaru engine.
// The model carries no weights: they come from the constraint selection (the
// components' kDefaultWeight by default).
//
// Modeling decisions (fixed by design, do not change casually):
//   - The single planning variable is the timeslot index (BASIC, -1 =
//     unassigned). Rooms are NOT a variable: same-typed special rooms are
//     interchangeable, so roomCapacityCost only compares per-(roomType, slot)
//     demand against the owned count (like the CP-SAT model; concrete rooms
//     are assigned in a trivial post-processing step outside the solver).
//   - One Lesson = one weekly period of one course (a course with
//     periods_per_week = 3 becomes 3 lessons sharing one courseKey).
//   - Parallel groups (split classes) do NOT exist in this domain. The
//   conversion
//     layer merges each parallel group into ONE "merged lesson" per weekly
//     period: teachers = union of the member courses' teachers, roomTypeDemand
//     = sum of the members' demands, and the merged lesson occupies exactly
//     one class slot (the group as a whole is one occupancy unit, matching
//     Course.occupancy_key in the CP-SAT domain). parallelGroupsCost (same
//     slots for all members) then holds by construction. The avoided /
//     non-preferred period costs stay exact through
//     Lesson::preferenceUnits: one (avoided, preferred) mask pair per member
//     course, each counted independently — a merged lesson penalizes exactly
//     like the member lessons it stands for in validate.py.
//   - Team teaching is a lesson with several entries in `teachers`; each
//     teacher occupies the slot individually (teacherConflictCost and the
//     teacher-mask family).
//   - Fixed slots are expressed as pinned lessons with a preassigned slot:
//     the engine never moves a pinned entity, so fixedSlotsCost is 0 by
//     construction.
//   - Everything that merely forbids a (lesson, slot) combination — calendar
//     holes, course forbidden_slots, teacher unavailable, class unavailable —
//     is pre-baked into the per-lesson allowedSlot table and costs 1 hard
//     point per violation (disallowedSlotCost).
namespace st::canon
{

using Score = optimaru::HardSoftScore;

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

    // Special-room demand per room type id (roomCapacityCost). Empty = no
    // demand; a merged lesson carries the SUM of its member courses'
    // demands.
    std::vector<int> roomTypeDemand;

    // Per-slot permission table (disallowedSlotCost), sized slotCount
    // (empty = every slot allowed). The conversion layer pre-bakes course
    // forbidden_slots,
    // teacher unavailable and class unavailable times into this table.
    std::vector<char> allowedSlot;

    // Preference units (avoidedPeriodCost / nonPreferredPeriodCost): one
    // (avoidedPeriodMask, preferredPeriodMask) pair per member course of a
    // merged lesson (bit = period - 1). Every
    // unit is counted independently per assigned period, reproducing the
    // per-member-lesson counting of validate.py's compute_penalties. Left
    // empty, prepare() derives the single default unit from the courseKey's
    // period masks (the plain, unmerged case).
    std::vector<std::pair<std::uint16_t, std::uint16_t>> preferenceUnits;

    // Block (consecutive periods) group id, -1 = none. All lessons sharing a
    // blockGroup must sit on one day in consecutive periods
    // (blockAdjacencyCost).
    int blockGroup = -1;

    // Pinned lessons keep their preassigned slot (fixed_slots, so
    // fixedSlotsCost is 0); the engine scores them but never moves them.
    bool pinned = false;

    int slot = -1;  // planning variable; -1 == unassigned
};

// ---------------------------------------------------------------------------
// Timetable  (@PlanningSolution)
// ---------------------------------------------------------------------------
// Slots are enumerated (day, period) pairs: dayOfSlot[s] is the 0-based day,
// periodOfSlot[s] the 1-based period of the day. Days may differ in length
// (e.g. Fri has 6 periods while Mon~Thu have 7) — simply enumerate the slots
// that exist. Periods must fit a 16-bit mask (period <= 16) and days a byte
// mask (day < 8).
struct Timetable
{
    std::vector<int> dayOfSlot;     // slot -> 0-based day
    std::vector<int> periodOfSlot;  // slot -> 1-based period
    std::vector<Lesson> lessons;

    int teacherCount = 0;  // may be left 0: prepare() derives max id + 1
    int classCount = 0;    // may be left 0: prepare() derives max id + 1

    // roomCapacityCost: owned special rooms per room type id.
    std::vector<int> roomCapacityByType;

    // Per-teacher limits (teacherMaxPeriodsCost,
    // teacherMaxConsecutivePeriodsCost), sized teacherCount by prepare() with
    // the CP-SAT defaults: maxPerDay 6, maxPerWeek -1 (unlimited),
    // maxConsecutive 4. teacherFreeDayCost: preferred free days as a
    // per-teacher day bitmask (bit d).
    std::vector<int> teacherMaxPerDay;
    std::vector<int> teacherMaxPerWeek;
    std::vector<int> teacherMaxConsecutive;
    std::vector<std::uint8_t> teacherPreferredFreeDays;

    // Per-courseKey data, sized courseCount by prepare(): the daily cap
    // (courseMaxPeriodsPerDayCost, default 1; a block course needs at least
    // its block size) and the subject period masks (avoidedPeriodCost /
    // nonPreferredPeriodCost, bit period-1; preferred mask 0 = no
    // preference).
    std::vector<int> courseDailyLimit;
    std::vector<std::uint16_t> courseAvoidedPeriodMask;
    std::vector<std::uint16_t> coursePreferredPeriodMask;

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
    // Lesson indices per block group (blockAdjacencyCost recomputes a whole
    // group at once).
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
// lessons (fixed slots) are excluded from every move.
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

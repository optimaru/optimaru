#ifndef ES_DOMAIN_HPP
#define ES_DOMAIN_HPP

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/api/score/buildin/hard_soft_score.hpp"
#include "optimaru/api/domain/solution_descriptor.hpp"

namespace es {

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
// A timestamp is int64 minutes from an arbitrary reference (day 0, 00:00), so
// all arithmetic (overlap, rest gap) is plain integer math. A calendar date is
// the day index = floor(minutes / MINUTES_PER_DAY). Shift times are never
// negative (ScoreCalculator::prepare rejects them), so day 0 is the first day
// of the problem, carried-in history included.
using Minutes = std::int64_t;
using DayIndex = std::int64_t;

constexpr Minutes MINUTES_PER_DAY = 24 * 60;

// The last day a shift may touch, about 100 years out. Per-day tables span the
// whole horizon, so one far-off shift would otherwise size them all.
constexpr DayIndex MAX_DAY = 36525;

inline DayIndex dayOf(Minutes m) { return m / MINUTES_PER_DAY; }

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The score type comes from the generic engine; hard penalties are
// whole numbers, soft carries the fractional load-balancing "unfairness".
using Score = optimaru::HardSoftScore;

// ---------------------------------------------------------------------------
// Employee
// ---------------------------------------------------------------------------
// Problem fact. Availability is expressed as sets of calendar days.
struct Employee {
    std::string name;
    std::set<std::string> skills;
    std::set<DayIndex> unavailableDates;
    std::set<DayIndex> undesiredDates;
    std::set<DayIndex> desiredDates;
    // Location-specific work preferences: (day, location) pairs the worker wants.
    // Rewarded only when assigned a shift of that location on that day (see the
    // DesiredShift constraint) — stricter than desiredDates, which rewards any
    // work that day. Kept after the 5 date/skill fields so 5-arg brace-inits and
    // the pybind Employee constructor still compile.
    std::set<std::pair<DayIndex, std::string>> desiredShifts;

    // Precomputed by ScoreCalculator::prepare: bit e set iff this employee has
    // skill id e (see the skill interning in prepare). Kept at the end so the
    // brace-initialisers elsewhere (5 fields) still compile.
    std::uint64_t skillMask = 0;
};

// ---------------------------------------------------------------------------
// Shift  (planning entity)
// ---------------------------------------------------------------------------
// employeeIndex is the basic planning variable: which employee works this shift.
// -1 means unassigned. There is no list variable and there are no shadow
// variables.
struct Shift {
    std::string id;
    Minutes start = 0;
    Minutes end = 0;
    std::string location;
    std::string requiredSkill;

    int employeeIndex = -1;  // planning variable; -1 == unassigned

    // Pinned shifts keep their employeeIndex fixed (history from the previous
    // planning period carried in for week-spanning rules, or an assignment that
    // is already settled). They are scored like any shift but never
    // moved/reassigned by the solver.
    bool pinned = false;

    // Precomputed by ScoreCalculator::prepare (kept at the end so the 6-field
    // brace-initialisers elsewhere still compile):
    int startDay = 0;                    // dayOf(start) -- avoids a division per pair
    int endDay = 0;                      // dayOf(end)
    std::uint64_t requiredSkillBit = 0;  // single bit for requiredSkill's id

    Minutes durationMinutes() const { return end - start; }
};

// ---------------------------------------------------------------------------
// EmployeeSchedule  (planning solution)
// ---------------------------------------------------------------------------
struct EmployeeSchedule {
    std::vector<Employee> employees;
    std::vector<Shift> shifts;
    Score score;
};

}  // namespace es

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization: entities are shifts, the planning variable
// is Shift::employeeIndex, the value range is the employee list.
// ---------------------------------------------------------------------------
template <>
struct optimaru::SolutionDescriptor<es::EmployeeSchedule> {
    static constexpr optimaru::VariableKind kind = optimaru::VariableKind::BASIC;
    static int entityCount(const es::EmployeeSchedule& s) {
        return static_cast<int>(s.shifts.size());
    }
    static int valueCount(const es::EmployeeSchedule& s) {
        return static_cast<int>(s.employees.size());
    }
    static int getVariable(const es::EmployeeSchedule& s, int shiftIndex) {
        return s.shifts[shiftIndex].employeeIndex;
    }
    static void setVariable(es::EmployeeSchedule& s, int shiftIndex, int employeeIndex) {
        s.shifts[shiftIndex].employeeIndex = employeeIndex;
    }
    static bool isPinned(const es::EmployeeSchedule& s, int shiftIndex) {
        return s.shifts[shiftIndex].pinned;
    }
};

#endif  // ES_DOMAIN_HPP

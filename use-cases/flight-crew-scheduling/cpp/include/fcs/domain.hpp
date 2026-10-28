#ifndef FCS_DOMAIN_HPP
#define FCS_DOMAIN_HPP

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace fcs
{

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
// The Java quickstart uses java.time.LocalDateTime (UTC) / LocalDate. A
// timestamp is int64 minutes from an arbitrary reference (day 0, 00:00 UTC),
// so overlap and ordering are plain integer math. A calendar date is the day
// index = floor(minutes / MINUTES_PER_DAY) — matching LocalDateTime
// .toLocalDate() as long as one consistent reference is used (the Python
// models use the Unix epoch).
using Minutes = std::int64_t;
using DayIndex = std::int64_t;

constexpr Minutes MINUTES_PER_DAY = 24 * 60;

inline DayIndex dayOf(Minutes m)
{
    // Floor division (minutes may in principle be negative).
    return m >= 0 ? m / MINUTES_PER_DAY
                  : -((-m + MINUTES_PER_DAY - 1) / MINUTES_PER_DAY);
}

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The Java quickstart scores with HardSoftScore (both levels int) — exactly
// the engine's HardSoftScore(int64 hard, double soft), so the mapping is the
// identity: no level folding is needed. The Java constraint weights are kept
// verbatim below.
using Score = optimaru::HardSoftScore;

// Java FlightCrewSchedulingConstraintProvider weights.
constexpr std::int64_t REQUIRED_SKILL_WEIGHT = 100;  // hard
constexpr std::int64_t FLIGHT_CONFLICT_WEIGHT = 10;  // hard
constexpr std::int64_t TRANSFER_WEIGHT = 1;          // hard
constexpr std::int64_t UNAVAILABILITY_WEIGHT = 10;   // hard
constexpr double FIRST_NOT_HOME_WEIGHT = 1000.0;     // soft
constexpr double LAST_NOT_HOME_WEIGHT = 1000.0;      // soft

// ---------------------------------------------------------------------------
// Airport  (problem fact)
// ---------------------------------------------------------------------------
// Referenced everywhere by index into FlightCrewSchedule::airports (the Java
// domain compares Airport objects by IATA code; an index plays that role).
// The Java Airport also carries taxiTimeInMinutes, which no constraint and no
// demo data uses — it is deliberately omitted here.
struct Airport
{
    std::string code;  // IATA 3-letter code
    std::string name;
    double latitude = 0.0;
    double longitude = 0.0;
};

// ---------------------------------------------------------------------------
// Employee  (problem fact, the value range)
// ---------------------------------------------------------------------------
struct Employee
{
    std::string id;
    std::string name;
    int homeAirport = -1;  // index into airports
    std::vector<std::string> skills;
    std::set<DayIndex> unavailableDays;
};

// ---------------------------------------------------------------------------
// Flight  (problem fact)
// ---------------------------------------------------------------------------
struct Flight
{
    std::string flightNumber;
    int departureAirport = -1;  // index into airports
    Minutes departureUTC = 0;
    int arrivalAirport = -1;  // index into airports
    Minutes arrivalUTC = 0;
};

// ---------------------------------------------------------------------------
// FlightAssignment  (@PlanningEntity)
// ---------------------------------------------------------------------------
// One crew seat on one flight. `employee` is the basic @PlanningVariable:
// which employee fills the seat. -1 means unassigned (employee == null).
struct FlightAssignment
{
    std::string id;
    int flight = -1;  // index into flights
    int indexInFlight = 0;
    std::string requiredSkill;

    int employee = -1;  // planning variable; -1 == unassigned

    // Pinned assignments keep their employee fixed (extension over the
    // quickstart, e.g. a captain confirmed on a leg). They are scored like
    // any assignment but never moved/reassigned by the solver.
    bool pinned = false;

    // Precomputed by ScoreCalculator::prepare (kept at the end so the
    // 4-field brace-initialisers elsewhere still compile):
    int requiredSkillId = -1;  // interned requiredSkill
};

// ---------------------------------------------------------------------------
// FlightCrewSchedule  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct FlightCrewSchedule
{
    std::vector<Airport> airports;
    std::vector<Employee> employees;
    std::vector<Flight> flights;
    std::vector<FlightAssignment> flightAssignments;

    Score score;

    // Set once by ScoreCalculator::prepare; guards against re-interning the
    // skill ids on every calculateScore call.
    bool prepared = false;

    // Filled by ScoreCalculator::prepare:
    int skillCount = 0;
    // employeeIndex * skillCount + skillId -> has skill
    std::vector<char> employeeSkillFlat;
    // Calendar-day window covered by the flights; unavailability outside the
    // window can never intersect a flight, so it is not stored.
    DayIndex minDay = 0;
    int dayCount = 0;
    // employeeIndex * dayCount + (day - minDay) -> unavailable
    std::vector<char> employeeUnavailableFlat;
    std::vector<DayIndex> flightDepDay;
    std::vector<DayIndex> flightArrDay;

    // --- prepared-lookup helpers ---
    bool employeeHasSkill(int employeeIndex, int skillId) const
    {
        return employeeSkillFlat[static_cast<std::size_t>(employeeIndex)
                                     * skillCount
                                 + skillId]
               != 0;
    }
    bool employeeUnavailableOn(int employeeIndex, DayIndex day) const
    {
        if (day < minDay || day >= minDay + dayCount)
            return false;
        return employeeUnavailableFlat[static_cast<std::size_t>(employeeIndex)
                                           * dayCount
                                       + static_cast<std::size_t>(day - minDay)]
               != 0;
    }

    // --- assignment-side convenience (mirrors the Java getters) ---
    Minutes departureOf(int assignmentIndex) const
    {
        return flights[flightAssignments[assignmentIndex].flight].departureUTC;
    }
    Minutes arrivalOf(int assignmentIndex) const
    {
        return flights[flightAssignments[assignmentIndex].flight].arrivalUTC;
    }
    int departureAirportOf(int assignmentIndex) const
    {
        return flights[flightAssignments[assignmentIndex].flight]
            .departureAirport;
    }
    int arrivalAirportOf(int assignmentIndex) const
    {
        return flights[flightAssignments[assignmentIndex].flight]
            .arrivalAirport;
    }
};

}  // namespace fcs

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization  (the @PlanningEntity/@PlanningVariable
// annotations of the C++ engine): entities are flight assignments, the
// planning variable is FlightAssignment::employee, the value range is the
// employee list.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<fcs::FlightCrewSchedule>
{
    static constexpr optimaru::VariableKind kind
        = optimaru::VariableKind::BASIC;
    static int entityCount(const fcs::FlightCrewSchedule &s)
    {
        return static_cast<int>(s.flightAssignments.size());
    }
    static int valueCount(const fcs::FlightCrewSchedule &s)
    {
        return static_cast<int>(s.employees.size());
    }
    static int getVariable(const fcs::FlightCrewSchedule &s,
                           int assignmentIndex)
    {
        return s.flightAssignments[assignmentIndex].employee;
    }
    static void setVariable(fcs::FlightCrewSchedule &s,
                            int assignmentIndex,
                            int employeeIndex)
    {
        s.flightAssignments[assignmentIndex].employee = employeeIndex;
    }
    static bool isPinned(const fcs::FlightCrewSchedule &s, int assignmentIndex)
    {
        return s.flightAssignments[assignmentIndex].pinned;
    }
};

#endif  // FCS_DOMAIN_HPP

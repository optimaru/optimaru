#ifndef FCS_SCORE_CALCULATOR_HPP
#define FCS_SCORE_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "fcs/domain.hpp"

namespace fcs
{

// Per-employee itinerary contribution: everything the pairwise constraints
// derive from one employee's set of assigned flights. Kept as raw counts;
// weights and signs are applied when the score is assembled.
struct ItineraryStats
{
    std::int64_t conflictPairs = 0;       // "Flight conflict"
    std::int64_t transferMismatches = 0;  // "Transfer between two flights"
    std::int64_t firstNotHome = 0;  // "First assignment not departing ..."
    std::int64_t lastNotHome = 0;   // "Last assignment not arriving ..."
};

// Port of the reference FlightCrewSchedulingConstraintProvider.
//
// calculateScore does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under FULL_ASSERT).
//
// Constraints (HardSoftScore, Java weights verbatim):
//   hard 1. requiredSkill        -100 per assignment whose employee lacks the
//                                     required skill
//   hard 2. flightConflict        -10 per same-employee pair of assignments
//                                     with time-overlapping flights
//                                     (aStart < bEnd && bStart < aEnd)
//   hard 3. transferBetweenTwoFlights
//                                  -1 per same-employee departure-consecutive
//                                     pair whose first arrival airport differs
//                                     from the second departure airport.
//                                     "Consecutive" follows the Java stream
//                                     exactly: pair (a, b) with dep(a) <
//                                     dep(b) and no third assignment c of the
//                                     employee with dep(a) <= dep(c) < dep(b)
//                                     — so an `a` sharing its departure time
//                                     with another assignment never starts a
//                                     pair, while ties on b all count.
//   hard 4. employeeUnavailability
//                                 -10 per assignment whose employee is
//                                     unavailable on any calendar day of the
//                                     flight ([depDate, arrDate] inclusive)
//   soft 5. firstAssignmentNotDepartingFromHome
//                               -1000 per assignment with the employee's
//                                     earliest departure time (ties all
//                                     count) not departing from home
//   soft 6. lastAssignmentNotArrivingAtHome
//                               -1000 per assignment with the employee's
//                                     latest departure time (ties all count)
//                                     not arriving at home
// Unassigned assignments (employee == -1) contribute nothing: the
// construction heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    static Score calculateScore(FlightCrewSchedule &schedule);

    // Interns skills into dense ids, flattens the employee skill sets and
    // unavailable days for O(1) lookup, and caches per-flight calendar days.
    // Idempotent (guarded by FlightCrewSchedule::prepared). Must run before
    // scoring; calculateScore/solve call it.
    static void prepare(FlightCrewSchedule &schedule);

    // --- shared building blocks (also used by the incremental calculator) --

    // Unary constraint predicates for one (assignment, employee) pairing.
    static bool missingRequiredSkill(const FlightCrewSchedule &schedule,
                                     int assignmentIndex,
                                     int employeeIndex);
    static bool unavailableEmployee(const FlightCrewSchedule &schedule,
                                    int assignmentIndex,
                                    int employeeIndex);

    // Sorts assignment indices by (departure time, assignment index).
    static void sortByDeparture(const FlightCrewSchedule &schedule,
                                std::vector<int> &assignmentIndices);

    // Computes the pairwise stats for one employee's assignments; the list
    // must already be sorted by sortByDeparture. O(n^2) worst case (n = the
    // employee's assignment count, small in practice).
    static ItineraryStats
    itineraryStats(const FlightCrewSchedule &schedule,
                   int employeeIndex,
                   const std::vector<int> &sortedAssignments);
};

}  // namespace fcs

#endif  // FCS_SCORE_CALCULATOR_HPP

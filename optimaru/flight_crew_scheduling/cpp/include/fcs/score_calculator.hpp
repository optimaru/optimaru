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
    std::int64_t conflictPairs = 0;       // flightConflictCost
    std::int64_t transferMismatches = 0;  // transferBetweenTwoFlightsCost
    std::int64_t firstNotHome = 0;  // firstAssignmentNotDepartingFromHomeCost
    std::int64_t lastNotHome = 0;   // lastAssignmentNotArrivingAtHomeCost
};

// The flight-crew-scheduling score as one monolithic function.
//
// score does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under ScoreChecks::Full).
//
// Constraints (Score), the default assembly of fcs/constraints.hpp at
// the components' kDefaultWeight:
//   hard 1. requiredSkillCost   -100 per assignment whose employee lacks the
//                                    required skill
//   hard 2. flightConflictCost   -10 per same-employee pair of assignments
//                                    with time-overlapping flights
//                                    (aStart < bEnd && bStart < aEnd)
//   hard 3. transferBetweenTwoFlightsCost
//                                 -1 per same-employee departure-consecutive
//                                    pair whose first arrival airport differs
//                                    from the second departure airport.
//                                    "Consecutive" means: pair (a, b) with dep(a) <
//                                    dep(b) and no third assignment c of the
//                                    employee with dep(a) <= dep(c) < dep(b)
//                                    — so an `a` sharing its departure time
//                                    with another assignment never starts a
//                                    pair, while ties on b all count.
//   hard 4. employeeUnavailabilityCost
//                                -10 per assignment whose employee is
//                                    unavailable on any calendar day of the
//                                    flight ([depDate, arrDate] inclusive)
//   soft 5. firstAssignmentNotDepartingFromHomeCost
//                              -1000 per assignment with the employee's
//                                    earliest departure time (ties all
//                                    count) not departing from home
//   soft 6. lastAssignmentNotArrivingAtHomeCost
//                              -1000 per assignment with the employee's
//                                    latest departure time (ties all count)
//                                    not arriving at home
// Unassigned assignments (employee == -1) contribute nothing: the
// construction heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    static Score score(FlightCrewSchedule &schedule);

    // Interns skills into dense ids, flattens the employee skill sets and
    // unavailable days for O(1) lookup, and caches per-flight calendar days.
    // Idempotent (guarded by FlightCrewSchedule::prepared). Must run before
    // scoring; score/solve call it.
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

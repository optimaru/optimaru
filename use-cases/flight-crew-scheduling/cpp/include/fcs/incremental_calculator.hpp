#ifndef FCS_INCREMENTAL_CALCULATOR_HPP
#define FCS_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "fcs/domain.hpp"
#include "fcs/score_calculator.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace fcs
{

// The flight-crew-scheduling implementation of the engine's
// optimaru::IncrementalScoreCalculator — the C++ analog of implementing
// Java's IncrementalScoreCalculator instead of a ConstraintProvider.
//
// Every constraint is a function of ONE employee's itinerary (plus two unary
// terms), so a single variable change is O(delta): it only touches the old
// and the new employee of the changed assignment.
//   - unary (requiredSkill, employeeUnavailability): O(1) flat-table lookups
//     added/removed with the assignment.
//   - pairwise (flightConflict, transfer, first/last-not-home): a per-employee
//     departure-sorted itinerary is maintained; when an assignment enters or
//     leaves an employee, that employee's ItineraryStats is recomputed from
//     the sorted list (O(n_e^2) worst case, n_e = that employee's assignment
//     count — a handful in practice) and the running totals take the diff.
//
// beforeVariableChanged retracts the assignment's contribution while
// `employee` still holds the OLD value; afterVariableChanged inserts the NEW
// value. Unassigned assignments (employee == -1) contribute nothing, so the
// construction heuristic can score partially initialized solutions. The
// full-recompute reference is ScoreCalculator::calculateScore (checked in the
// tests and under EnvironmentMode::FULL_ASSERT).
class FlightCrewIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<FlightCrewSchedule>
{
public:
    void resetWorkingSolution(FlightCrewSchedule &workingSolution) override;
    void beforeVariableChanged(int assignmentIndex) override;
    void afterVariableChanged(int assignmentIndex) override;
    Score calculateScore() override;

private:
    FlightCrewSchedule *schedule_ = nullptr;
    std::vector<int> assign_;  // employee per assignment (-1 == unassigned)

    // Departure-sorted assignment indices per employee, plus that employee's
    // cached pairwise stats.
    std::vector<std::vector<int>> byEmployee_;
    std::vector<ItineraryStats> stats_;

    // Running totals (all counted positive; signs applied in calculateScore).
    std::int64_t missingSkill_ = 0;
    std::int64_t unavailable_ = 0;
    std::int64_t conflictPairs_ = 0;
    std::int64_t transfers_ = 0;
    std::int64_t firstNotHome_ = 0;
    std::int64_t lastNotHome_ = 0;

    void insert(int assignmentIndex, int employeeIndex);
    void retract(int assignmentIndex, int employeeIndex);
    void refreshEmployee(int employeeIndex);
};

}  // namespace fcs

#endif  // FCS_INCREMENTAL_CALCULATOR_HPP

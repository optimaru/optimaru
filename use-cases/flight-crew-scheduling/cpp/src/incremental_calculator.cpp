#include "fcs/incremental_calculator.hpp"

#include <algorithm>
#include <cstdint>

#include "fcs/constraints/employee_unavailability_cost.hpp"
#include "fcs/constraints/first_assignment_not_departing_from_home_cost.hpp"
#include "fcs/constraints/flight_conflict_cost.hpp"
#include "fcs/constraints/last_assignment_not_arriving_at_home_cost.hpp"
#include "fcs/constraints/required_skill_cost.hpp"
#include "fcs/constraints/transfer_between_two_flights_cost.hpp"

namespace fcs
{

namespace
{

// The same constants the components and ScoreCalculator read (integral hard
// weights; see score_calculator.cpp).
constexpr auto kRequiredSkillWeight
    = static_cast<std::int64_t>(RequiredSkillCost::kDefaultWeight);
constexpr auto kFlightConflictWeight
    = static_cast<std::int64_t>(FlightConflictCost::kDefaultWeight);
constexpr auto kTransferWeight
    = static_cast<std::int64_t>(TransferBetweenTwoFlightsCost::kDefaultWeight);
constexpr auto kUnavailabilityWeight
    = static_cast<std::int64_t>(EmployeeUnavailabilityCost::kDefaultWeight);
constexpr double kFirstNotHomeWeight
    = FirstAssignmentNotDepartingFromHomeCost::kDefaultWeight;
constexpr double kLastNotHomeWeight
    = LastAssignmentNotArrivingAtHomeCost::kDefaultWeight;

}  // namespace

void FlightCrewIncrementalScoreCalculator::resetWorkingSolution(
    FlightCrewSchedule &workingSolution)
{
    ScoreCalculator::prepare(workingSolution);
    schedule_ = &workingSolution;

    const int assignmentCount
        = static_cast<int>(workingSolution.flightAssignments.size());
    assign_.assign(assignmentCount, -1);
    byEmployee_.assign(workingSolution.employees.size(), {});
    stats_.assign(workingSolution.employees.size(), ItineraryStats{});
    missingSkill_ = 0;
    unavailable_ = 0;
    conflictPairs_ = 0;
    transfers_ = 0;
    firstNotHome_ = 0;
    lastNotHome_ = 0;

    for (int i = 0; i < assignmentCount; ++i)
    {
        const int employee = workingSolution.flightAssignments[i].employee;
        if (employee >= 0)
            insert(i, employee);
    }
}

void FlightCrewIncrementalScoreCalculator::beforeVariableChanged(
    int assignmentIndex)
{
    const int oldEmployee = assign_[assignmentIndex];
    if (oldEmployee >= 0)
        retract(assignmentIndex, oldEmployee);
}

void FlightCrewIncrementalScoreCalculator::afterVariableChanged(
    int assignmentIndex)
{
    const int newEmployee
        = schedule_->flightAssignments[assignmentIndex].employee;
    if (newEmployee >= 0)
        insert(assignmentIndex, newEmployee);
    else
        assign_[assignmentIndex] = -1;
}

void FlightCrewIncrementalScoreCalculator::insert(int assignmentIndex,
                                                  int employeeIndex)
{
    assign_[assignmentIndex] = employeeIndex;

    // Unary terms.
    if (ScoreCalculator::missingRequiredSkill(
            *schedule_, assignmentIndex, employeeIndex))
        ++missingSkill_;
    if (ScoreCalculator::unavailableEmployee(
            *schedule_, assignmentIndex, employeeIndex))
        ++unavailable_;

    // Keep the itinerary sorted by (departure, index) and refresh the
    // employee's pairwise stats.
    std::vector<int> &itinerary = byEmployee_[employeeIndex];
    const Minutes dep = schedule_->departureOf(assignmentIndex);
    auto it = std::lower_bound(itinerary.begin(),
                               itinerary.end(),
                               assignmentIndex,
                               [this, dep](int existing, int inserted)
                               {
                                   const Minutes existingDep
                                       = schedule_->departureOf(existing);
                                   if (existingDep != dep)
                                       return existingDep < dep;
                                   return existing < inserted;
                               });
    itinerary.insert(it, assignmentIndex);
    refreshEmployee(employeeIndex);
}

void FlightCrewIncrementalScoreCalculator::retract(int assignmentIndex,
                                                   int employeeIndex)
{
    assign_[assignmentIndex] = -1;

    if (ScoreCalculator::missingRequiredSkill(
            *schedule_, assignmentIndex, employeeIndex))
        --missingSkill_;
    if (ScoreCalculator::unavailableEmployee(
            *schedule_, assignmentIndex, employeeIndex))
        --unavailable_;

    std::vector<int> &itinerary = byEmployee_[employeeIndex];
    itinerary.erase(
        std::find(itinerary.begin(), itinerary.end(), assignmentIndex));
    refreshEmployee(employeeIndex);
}

void FlightCrewIncrementalScoreCalculator::refreshEmployee(int employeeIndex)
{
    const ItineraryStats &old = stats_[employeeIndex];
    conflictPairs_ -= old.conflictPairs;
    transfers_ -= old.transferMismatches;
    firstNotHome_ -= old.firstNotHome;
    lastNotHome_ -= old.lastNotHome;

    const ItineraryStats fresh = ScoreCalculator::itineraryStats(
        *schedule_, employeeIndex, byEmployee_[employeeIndex]);
    stats_[employeeIndex] = fresh;
    conflictPairs_ += fresh.conflictPairs;
    transfers_ += fresh.transferMismatches;
    firstNotHome_ += fresh.firstNotHome;
    lastNotHome_ += fresh.lastNotHome;
}

Score FlightCrewIncrementalScoreCalculator::calculateScore()
{
    Score score;
    score.hard = -(kRequiredSkillWeight * missingSkill_
                   + kFlightConflictWeight * conflictPairs_
                   + kTransferWeight * transfers_
                   + kUnavailabilityWeight * unavailable_);
    score.soft = -(kFirstNotHomeWeight * static_cast<double>(firstNotHome_)
                   + kLastNotHomeWeight * static_cast<double>(lastNotHome_));
    if (score.soft == 0.0)
        score.soft = 0.0;  // normalize -0.0
    return score;
}

}  // namespace fcs

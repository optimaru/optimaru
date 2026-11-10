#ifndef FCS_SRC_MONOLITHIC_WEIGHTS_HPP
#define FCS_SRC_MONOLITHIC_WEIGHTS_HPP

// Private to the two monolithic calculators (score_calculator.cpp and
// incremental_calculator.cpp); not part of the public include/ tree.
//
// The monolithic score is the default assembly (fcs/constraints.hpp), so both
// calculators read the same constants the components carry, from here, and
// the checks below hold for every translation unit that uses them. The hard
// weights are integral, so the hard level is computed exactly, in
// int64; the soft weights stay doubles.

#include <cstdint>

#include "fcs/constraints/employee_unavailability_cost.hpp"
#include "fcs/constraints/first_assignment_not_departing_from_home_cost.hpp"
#include "fcs/constraints/flight_conflict_cost.hpp"
#include "fcs/constraints/last_assignment_not_arriving_at_home_cost.hpp"
#include "fcs/constraints/required_skill_cost.hpp"
#include "fcs/constraints/transfer_between_two_flights_cost.hpp"

namespace fcs
{

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

static_assert(kRequiredSkillWeight == RequiredSkillCost::kDefaultWeight
                  && kFlightConflictWeight == FlightConflictCost::kDefaultWeight
                  && kTransferWeight
                         == TransferBetweenTwoFlightsCost::kDefaultWeight
                  && kUnavailabilityWeight
                         == EmployeeUnavailabilityCost::kDefaultWeight,
              "the monolithic calculators assume integral hard weights");
static_assert(
    RequiredSkillCost::kDefaultLevel == optimaru::Level::Hard
        && FlightConflictCost::kDefaultLevel == optimaru::Level::Hard
        && TransferBetweenTwoFlightsCost::kDefaultLevel == optimaru::Level::Hard
        && EmployeeUnavailabilityCost::kDefaultLevel == optimaru::Level::Hard
        && FirstAssignmentNotDepartingFromHomeCost::kDefaultLevel
               == optimaru::Level::Soft
        && LastAssignmentNotArrivingAtHomeCost::kDefaultLevel
               == optimaru::Level::Soft,
    "the monolithic calculators assume the component levels");

}  // namespace fcs

#endif  // FCS_SRC_MONOLITHIC_WEIGHTS_HPP

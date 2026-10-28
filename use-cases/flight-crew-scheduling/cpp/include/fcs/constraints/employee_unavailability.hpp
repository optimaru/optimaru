#ifndef FCS_CONSTRAINTS_EMPLOYEE_UNAVAILABILITY_HPP
#define FCS_CONSTRAINTS_EMPLOYEE_UNAVAILABILITY_HPP

#include "fcs/constraints/common.hpp"

namespace fcs
{

// Hard (default, weight 10): employeeUnavailability — an assignment whose
// employee is unavailable on any calendar day the flight spans (departure
// through arrival date, inclusive).
struct EmployeeUnavailability final : PerAssignmentConstraint
{
    explicit EmployeeUnavailability(double weight = 10.0)
        : PerAssignmentConstraint("employeeUnavailability",
                                  weight,
                                  /*hard=*/true)
    {
    }
    bool violated(const FlightCrewSchedule &schedule,
                  int assignmentIndex,
                  int employeeIndex) const override
    {
        return ScoreCalculator::unavailableEmployee(
            schedule, assignmentIndex, employeeIndex);
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_EMPLOYEE_UNAVAILABILITY_HPP

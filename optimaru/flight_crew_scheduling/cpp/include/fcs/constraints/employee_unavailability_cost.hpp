#ifndef FCS_CONSTRAINTS_EMPLOYEE_UNAVAILABILITY_COST_HPP
#define FCS_CONSTRAINTS_EMPLOYEE_UNAVAILABILITY_COST_HPP

#include "fcs/constraints/common.hpp"

namespace fcs {

// Hard, weight 10: employeeUnavailabilityCost — an assignment whose employee
// is unavailable on any calendar day the flight spans (departure through
// arrival date, inclusive). Raw cost per employee = the number of such
// assignments (a count, not minutes as in employee-scheduling), so 0 = nobody
// flies on a day they are unavailable.
struct EmployeeUnavailabilityCost final : PerAssignmentConstraint {
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit EmployeeUnavailabilityCost(double weight = kDefaultWeight)
        : PerAssignmentConstraint("employeeUnavailabilityCost", weight, kDefaultLevel) {}
    bool violated(const FlightCrewSchedule& schedule, int assignmentIndex,
                  int employeeIndex) const override {
        return ScoreCalculator::unavailableEmployee(schedule, assignmentIndex, employeeIndex);
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_EMPLOYEE_UNAVAILABILITY_COST_HPP

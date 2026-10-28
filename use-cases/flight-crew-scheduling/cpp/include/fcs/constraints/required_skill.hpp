#ifndef FCS_CONSTRAINTS_REQUIRED_SKILL_HPP
#define FCS_CONSTRAINTS_REQUIRED_SKILL_HPP

#include "fcs/constraints/common.hpp"

namespace fcs
{

// Hard (default, weight 100): requiredSkill — an assignment whose employee
// does not hold the skill the seat requires. The heaviest hard rule of the
// provider: a crew member who cannot fly the seat is worse than any schedule
// awkwardness.
struct RequiredSkill final : PerAssignmentConstraint
{
    explicit RequiredSkill(double weight = 100.0)
        : PerAssignmentConstraint("requiredSkill", weight, /*hard=*/true)
    {
    }
    bool violated(const FlightCrewSchedule &schedule,
                  int assignmentIndex,
                  int employeeIndex) const override
    {
        return ScoreCalculator::missingRequiredSkill(
            schedule, assignmentIndex, employeeIndex);
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_REQUIRED_SKILL_HPP

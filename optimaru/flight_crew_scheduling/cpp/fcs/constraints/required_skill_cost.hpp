#ifndef FCS_CONSTRAINTS_REQUIRED_SKILL_COST_HPP
#define FCS_CONSTRAINTS_REQUIRED_SKILL_COST_HPP

#include "fcs/constraints/common.hpp"

namespace fcs {

// Hard, weight 100: requiredSkillCost — an assignment whose employee does not
// hold the skill the seat requires. Raw cost per employee = the number of
// their assignments whose required skill they lack, so 0 = every seat is
// flown by a qualified crew member. The heaviest hard rule
// (weight 100): a crew member who cannot fly the seat is worse than
// any schedule awkwardness.
struct RequiredSkillCost final : PerAssignmentConstraint {
    static constexpr double kDefaultWeight = 100.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RequiredSkillCost(double weight = kDefaultWeight)
        : PerAssignmentConstraint("requiredSkillCost", weight, kDefaultLevel) {}
    bool violated(const FlightCrewSchedule& schedule, int assignmentIndex,
                  int employeeIndex) const override {
        return ScoreCalculator::missingRequiredSkill(schedule, assignmentIndex, employeeIndex);
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_REQUIRED_SKILL_COST_HPP

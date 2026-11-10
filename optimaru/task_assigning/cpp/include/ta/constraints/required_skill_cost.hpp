#ifndef TA_CONSTRAINTS_REQUIRED_SKILL_COST_HPP
#define TA_CONSTRAINTS_REQUIRED_SKILL_COST_HPP

#include "ta/constraints/common.hpp"

namespace ta
{

// Hard, weight 1: requiredSkillCost — the skills a task's type requires that
// its employee does not have. Raw cost per employee = the number of missing
// required skills summed over the employee's tasks (one task can add more
// than 1), so 0 = every assigned task is fully skilled. The Bendable hard[0]
// level: the domain's below-hard medium fold (domain.hpp, kMediumFold) puts
// one hard unit kMissingSkillUnit (10^9) engine-hard units above one
// unassigned task, so the weight stays in plain hard units.
struct RequiredSkillCost final : TaUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RequiredSkillCost(double weight = kDefaultWeight)
        : TaUnitConstraint("requiredSkillCost", weight, kDefaultLevel)
    {
    }
    double cost(const TaskAssigningSolution &solution,
                int unit,
                const std::vector<int> &members) const override
    {
        std::int64_t missing = 0;
        for (int taskIndex : members)
            missing
                += solution.missingSkillCount(solution.tasks[taskIndex], unit);
        return static_cast<double>(missing);
    }
};

}  // namespace ta

#endif  // TA_CONSTRAINTS_REQUIRED_SKILL_COST_HPP

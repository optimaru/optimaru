#ifndef TA_CONSTRAINTS_NO_MISSING_SKILLS_HPP
#define TA_CONSTRAINTS_NO_MISSING_SKILLS_HPP

#include "ta/constraints/common.hpp"

namespace ta
{

// Hard (default, weight kMissingSkillUnit): noMissingSkills — one point per
// skill a task's type requires and its employee does not have. The Bendable
// hard[0] level, which the engine's folded hard level carries above the
// unassigned-task count (see common.hpp).
struct NoMissingSkills final : TaUnitConstraint
{
    explicit NoMissingSkills(double weight
                             = static_cast<double>(kMissingSkillUnit))
        : TaUnitConstraint("noMissingSkills", weight, /*hard=*/true)
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

#endif  // TA_CONSTRAINTS_NO_MISSING_SKILLS_HPP

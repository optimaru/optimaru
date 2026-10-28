#ifndef TA_CONSTRAINTS_MINIMIZE_UNASSIGNED_TASKS_HPP
#define TA_CONSTRAINTS_MINIMIZE_UNASSIGNED_TASKS_HPP

#include "ta/constraints/common.hpp"

namespace ta
{

// Hard (default, weight 1): minimizeUnassignedTasks — one point per task in
// no employee's list. The Bendable soft[0] level, which the engine's folded
// hard level carries BELOW the missing-skill count (see common.hpp), so it
// stays lexicographically dominated exactly like in Java.
//
// Global because an unassigned task belongs to no unit (the
// school-timetabling UnassignedLesson pattern). It tracks each list's size,
// so a touched list costs O(1).
struct MinimizeUnassignedTasks final : TaGlobalConstraint
{
    explicit MinimizeUnassignedTasks(double weight = 1.0)
        : TaGlobalConstraint("minimizeUnassignedTasks", weight, /*hard=*/true)
    {
    }
    void reset(const TaskAssigningSolution &solution,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        taskCount_ = static_cast<std::int64_t>(solution.tasks.size());
        sizes_.assign(membersPerUnit.size(), 0);
        assigned_ = 0;
        for (std::size_t unit = 0; unit < membersPerUnit.size(); ++unit)
        {
            sizes_[unit]
                = static_cast<std::int64_t>(membersPerUnit[unit].size());
            assigned_ += sizes_[unit];
        }
    }
    void onUnitChanged(const TaskAssigningSolution & /*solution*/,
                       int unit,
                       const std::vector<int> &members) override
    {
        const std::int64_t size = static_cast<std::int64_t>(members.size());
        assigned_ += size - sizes_[unit];
        sizes_[unit] = size;
    }
    double cost() const override
    {
        return static_cast<double>(taskCount_ - assigned_);
    }

private:
    std::int64_t taskCount_ = 0;
    std::int64_t assigned_ = 0;
    std::vector<std::int64_t> sizes_;  // per unit: current list size
};

}  // namespace ta

#endif  // TA_CONSTRAINTS_MINIMIZE_UNASSIGNED_TASKS_HPP

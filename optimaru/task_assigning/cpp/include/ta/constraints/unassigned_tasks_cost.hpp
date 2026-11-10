#ifndef TA_CONSTRAINTS_UNASSIGNED_TASKS_COST_HPP
#define TA_CONSTRAINTS_UNASSIGNED_TASKS_COST_HPP

#include "ta/constraints/common.hpp"

namespace ta
{

// Medium, weight 1: unassignedTasksCost — the tasks in no employee's list.
// Raw cost = the number of unassigned tasks, so 0 = every task is assigned.
// The Bendable soft[0] level, the domain's medium level: the below-hard fold
// (domain.hpp, kMediumFold) reports engine.hard = hard * 10^9 + medium, so it
// stays lexicographically below the missing-skill count exactly like in
// Java. Its weight is in medium units; hard() makes it 1 hard unit per task,
// soft() 1 soft unit per task (outside the fold).
//
// Global because an unassigned task belongs to no unit (the
// school-timetabling UnassignedLessonsCost pattern). It tracks each list's
// size, so a touched list costs O(1).
struct UnassignedTasksCost final : TaGlobalConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Medium;

    explicit UnassignedTasksCost(double weight = kDefaultWeight)
        : TaGlobalConstraint("unassignedTasksCost", weight, kDefaultLevel)
    {
    }
    void reset(const TaskAssigningSolution &solution,
               const std::vector<std::vector<int>> &members_per_unit) override
    {
        taskCount_ = static_cast<std::int64_t>(solution.tasks.size());
        sizes_.assign(members_per_unit.size(), 0);
        assigned_ = 0;
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit)
        {
            sizes_[unit]
                = static_cast<std::int64_t>(members_per_unit[unit].size());
            assigned_ += sizes_[unit];
        }
    }
    void on_unit_changed(const TaskAssigningSolution & /*solution*/,
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

#endif  // TA_CONSTRAINTS_UNASSIGNED_TASKS_COST_HPP

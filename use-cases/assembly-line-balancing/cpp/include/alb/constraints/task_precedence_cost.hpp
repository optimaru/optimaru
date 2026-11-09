#ifndef ALB_CONSTRAINTS_TASK_PRECEDENCE_COST_HPP
#define ALB_CONSTRAINTS_TASK_PRECEDENCE_COST_HPP

#include <cstdint>
#include <vector>

#include "alb/constraints/common.hpp"

namespace alb
{

// Hard, weight 1: taskPrecedenceCost — precedence arcs (before, after) whose
// `before` task sits on a LATER station than its `after` task. Raw cost = the
// number of such arcs (a duplicate arc counts once per copy; a self-arc never
// costs anything), counted only while both tasks are assigned; 0 = the line
// respects every precedence. One total (a global), so the exponent shapes
// that count.
//
// Member-change fast path: a task joining or leaving a station adds or
// removes the arcs touching it, evaluated against the other task's station as
// this component last saw it (its own mirror, so a leave is exactly the
// inverse of the matching join): O(degree) per change.
struct TaskPrecedenceCost final : MemberChangeGlobalConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TaskPrecedenceCost(double weight = kDefaultWeight)
        : MemberChangeGlobalConstraint("taskPrecedenceCost", weight, kDefaultLevel)
    {
    }

    void reset(const AssemblyLineBalancingProblem &problem,
               const std::vector<std::vector<int>> &members_per_unit) override
    {
        detail::requirePrepared(problem);
        station_.assign(problem.tasks.size(), -1);
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit)
        {
            for (int task : members_per_unit[unit])
                station_[task] = static_cast<int>(unit);
        }
        reversed_ = 0;
        for (const Precedence &arc : problem.precedences)
        {
            const int before = station_[arc.before];
            const int after = station_[arc.after];
            if (arc.before != arc.after && before >= 0 && after >= 0 && before > after)
                ++reversed_;
        }
    }

    void on_member_changed(const AssemblyLineBalancingProblem &problem,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int entity,
                         bool joined) override
    {
        if (joined)
        {
            station_[entity] = unit;
            reversed_ += reversedArcsOf(problem, entity, unit);
        }
        else
        {
            reversed_ -= reversedArcsOf(problem, entity, unit);
            station_[entity] = -1;
        }
    }

    double cost() const override { return static_cast<double>(reversed_); }

private:
    // Reversed arcs touching `task` while it sits on `station`, against the
    // mirrored stations of the other endpoints (unassigned ones never count).
    std::int64_t reversedArcsOf(const AssemblyLineBalancingProblem &problem,
                                int task,
                                int station) const
    {
        std::int64_t count = 0;
        for (int arc : problem.arcsOut[task])
        {
            const int after = station_[problem.precedences[arc].after];
            if (after >= 0 && station > after)
                ++count;
        }
        for (int arc : problem.arcsIn[task])
        {
            const int before = station_[problem.precedences[arc].before];
            if (before >= 0 && before > station)
                ++count;
        }
        return count;
    }

    std::vector<int> station_;  // mirror: station per task, -1 = unassigned
    std::int64_t reversed_ = 0;
};

}  // namespace alb

#endif  // ALB_CONSTRAINTS_TASK_PRECEDENCE_COST_HPP

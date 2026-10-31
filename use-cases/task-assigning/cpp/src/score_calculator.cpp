#include "ta/score_calculator.hpp"

#include <algorithm>

#include "ta/constraints.hpp"

namespace ta
{

namespace
{
// The monolith scores every level at the components' default weights: the
// priority factors come from priorityWeight() (constraints.hpp), the hard,
// medium and soft[1] levels count one unit per raw unit, and the Score
// adapter (toEngineScore) applies the medium fold and the soft[1] packing.
static_assert(RequiredSkillCost::kDefaultWeight == 1.0
                  && UnassignedTasksCost::kDefaultWeight == 1.0
                  && SquaredMakespanCost::kDefaultWeight
                         == static_cast<double>(kMakespanUnit),
              "the monolithic calculators assume the component defaults");
static_assert(RequiredSkillCost::kDefaultLevel == optimaru::Level::Hard
                  && UnassignedTasksCost::kDefaultLevel
                         == optimaru::Level::Medium
                  && SquaredMakespanCost::kDefaultLevel
                         == optimaru::Level::Soft,
              "the monolithic calculators assume the component levels");
}  // namespace

void ScoreCalculator::updateShadowVariables(TaskAssigningSolution &solution)
{
    for (Task &task : solution.tasks)
    {
        task.employee = -1;
        task.startTime = kNullTime;
    }
    for (std::size_t e = 0; e < solution.employees.size(); ++e)
    {
        const Employee &employee = solution.employees[e];
        Minutes previousEndTime = 0;
        bool first = true;
        for (int taskIndex : employee.tasks)
        {
            Task &task = solution.tasks[taskIndex];
            task.employee = static_cast<int>(e);
            // Task.startTimeSupplier: the first task starts at its own
            // minStartTime; later tasks wait for the previous end.
            task.startTime = first
                                 ? task.minStartTime
                                 : std::max(previousEndTime, task.minStartTime);
            previousEndTime
                = task.startTime
                  + solution.taskDuration(task, static_cast<int>(e));
            first = false;
        }
    }
}

Score ScoreCalculator::calculateScore(TaskAssigningSolution &solution)
{
    updateShadowVariables(solution);

    std::int64_t missingSkillTotal = 0;     // hard[0]  requiredSkillCost
    std::int64_t unassignedTotal = 0;       // soft[0]  unassignedTasksCost
    std::int64_t makespanSquaredTotal = 0;  // soft[1]  squaredMakespanCost
    std::int64_t weightedEndTimeTotal = 0;  // soft[2]  *PriorityTaskEndTimeCost

    for (const Employee &employee : solution.employees)
    {
        Minutes employeeEndTime = 0;  // Employee.getEndTime: 0 when empty
        for (int taskIndex : employee.tasks)
        {
            const Task &task = solution.tasks[taskIndex];
            const Minutes endTime
                = task.startTime + solution.taskDuration(task, task.employee);
            missingSkillTotal
                += solution.missingSkillCount(task, task.employee);
            weightedEndTimeTotal += priorityWeight(task.priority) * endTime;
            employeeEndTime = endTime;  // the LAST task's end time
        }
        // squaredMakespanCost: every employee penalizes endTime^2 (0 for an
        // employee without tasks).
        makespanSquaredTotal += employeeEndTime * employeeEndTime;
    }
    for (const Task &task : solution.tasks)
    {
        if (task.employee < 0)
            ++unassignedTotal;
    }

    solution.score = {-missingSkillTotal,
                      -unassignedTotal,
                      -makespanSquaredTotal,
                      -weightedEndTimeTotal};
    return solution.score;
}

optimaru::HardSoftScore
ScoreCalculator::calculateEngineScore(TaskAssigningSolution &solution)
{
    return toEngineScore(calculateScore(solution));
}

}  // namespace ta

#include "ta/score_calculator.hpp"

#include <algorithm>

namespace ta
{

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

    std::int64_t missingSkillTotal = 0;     // hard[0]
    std::int64_t unassignedTotal = 0;       // soft[0]
    std::int64_t makespanSquaredTotal = 0;  // soft[1]
    std::int64_t weightedEndTimeTotal = 0;  // soft[2]

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
        // minimizeMakespan: every employee penalizes endTime^2 (0 for an
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

#include "ta/incremental_calculator.hpp"

#include <algorithm>

namespace ta
{

void TaskAssigningIncrementalScoreCalculator::resetWorkingSolution(
    TaskAssigningSolution &workingSolution)
{
    solution_ = &workingSolution;
    const int employeeCount
        = static_cast<int>(workingSolution.employees.size());
    const int typeCount = static_cast<int>(workingSolution.taskTypes.size());
    const int taskCount = static_cast<int>(workingSolution.tasks.size());

    // Intern the string-based lookups: missing-skill counts per (employee,
    // task type) and durations per (employee, task).
    missingByEmployeeType_.assign(employeeCount, std::vector<int>(typeCount));
    durationByEmployeeTask_.assign(employeeCount,
                                   std::vector<Minutes>(taskCount));
    for (int e = 0; e < employeeCount; ++e)
    {
        for (int t = 0; t < typeCount; ++t)
        {
            // Any task of type t has the same missing-skill count for e.
            Task probe;
            probe.taskType = t;
            missingByEmployeeType_[e][t]
                = workingSolution.missingSkillCount(probe, e);
        }
        for (int i = 0; i < taskCount; ++i)
        {
            durationByEmployeeTask_[e][i]
                = workingSolution.taskDuration(workingSolution.tasks[i], e);
        }
    }

    contribMissing_.assign(employeeCount, 0);
    contribWeightedEnd_.assign(employeeCount, 0);
    endTime_.assign(employeeCount, 0);
    missingTotal_ = 0;
    weightedEndTotal_ = 0;
    makespanSquaredTotal_ = 0;
    assignedCount_ = 0;
    for (int e = 0; e < employeeCount; ++e)
        insertList(e);
}

void TaskAssigningIncrementalScoreCalculator::beforeListChanged(int listIndex)
{
    // Retract the list's cached aggregates while it still holds its OLD
    // contents.
    missingTotal_ -= contribMissing_[listIndex];
    weightedEndTotal_ -= contribWeightedEnd_[listIndex];
    makespanSquaredTotal_ -= endTime_[listIndex] * endTime_[listIndex];
    assignedCount_ -= static_cast<std::int64_t>(
        solution_->employees[listIndex].tasks.size());
}

void TaskAssigningIncrementalScoreCalculator::afterListChanged(int listIndex)
{
    insertList(listIndex);
}

void TaskAssigningIncrementalScoreCalculator::insertList(int listIndex)
{
    const Employee &employee = solution_->employees[listIndex];
    std::int64_t missing = 0;
    std::int64_t weightedEnd = 0;
    Minutes endTime = 0;
    bool first = true;
    for (int taskIndex : employee.tasks)
    {
        const Task &task = solution_->tasks[taskIndex];
        const Minutes startTime
            = first ? task.minStartTime : std::max(endTime, task.minStartTime);
        endTime = startTime + durationByEmployeeTask_[listIndex][taskIndex];
        first = false;
        missing += missingByEmployeeType_[listIndex][task.taskType];
        weightedEnd += priorityWeight(task.priority) * endTime;
    }
    contribMissing_[listIndex] = missing;
    contribWeightedEnd_[listIndex] = weightedEnd;
    endTime_[listIndex] = endTime;
    missingTotal_ += missing;
    weightedEndTotal_ += weightedEnd;
    makespanSquaredTotal_ += endTime * endTime;
    assignedCount_ += static_cast<std::int64_t>(employee.tasks.size());
}

optimaru::HardSoftScore
TaskAssigningIncrementalScoreCalculator::calculateScore()
{
    const std::int64_t unassigned
        = static_cast<std::int64_t>(solution_->tasks.size()) - assignedCount_;
    return toEngineScore(Score{-missingTotal_,
                               -unassigned,
                               -makespanSquaredTotal_,
                               -weightedEndTotal_});
}

}  // namespace ta

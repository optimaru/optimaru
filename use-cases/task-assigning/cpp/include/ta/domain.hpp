#ifndef TA_DOMAIN_HPP
#define TA_DOMAIN_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace ta
{

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
// The Java quickstart counts minutes as plain longs (Task.startTime /
// TaskType.baseDuration are "in minutes"); so do we.
using Minutes = std::int64_t;

constexpr Minutes kNullTime = -1;  // stands in for a null startTime (Long)

// ---------------------------------------------------------------------------
// Score  (BendableScore [1]hard/[3]soft mirror)
// ---------------------------------------------------------------------------
// The Java quickstart scores with BendableScore(hardLevels=1, softLevels=3):
//   hard[0]  noMissingSkills            (missing skill count)
//   soft[0]  minimizeUnassignedTasks    (1 per unassigned task)
//   soft[1]  minimizeMakespan           (employee endTime^2 each)
//   soft[2]  {critical|major|minor}PriorityTaskEndTime
//            (endTime x 4 / x 2 / x 1)
// The generic engine (include/optimaru/) only knows HardSoftScore(int64 hard, double
// soft). Mapping rule (level priority preserved, cf. the bed-allocation
// HardMediumSoft mapping with a 10^9 offset):
//
//   engine.hard = hard * kMissingSkillUnit + soft0   (kMissingSkillUnit=10^9)
//   engine.soft = soft1 * kMakespanUnit + soft2      (kMakespanUnit    =10^7)
//
// All four levels penalize whole units (skill counts / task counts / squared
// minutes / weighted minutes), so every component is a non-positive integer:
//  - engine.hard is exact int64 arithmetic; hard dominates soft0 as long as
//    |soft0| < 10^9, i.e. fewer than 10^9 tasks.
//  - engine.soft is exact inside the double's 2^53 integer range as long as
//    |soft1| < 9 * 10^8 (sum of squared employee end-minutes) and soft1
//    dominates soft2 as long as |soft2| < 10^7 (weighted end-minute sum).
//    That covers instances whose horizon is tens of thousands of minutes —
//    the same practical range where the Java int-based BendableScore does
//    not overflow either.
// One semantic wrinkle: the engine's isFeasible() (engine.hard >= 0) now
// also requires soft0 == 0. Because the engine's LIST local search keeps
// every task assigned once construction finished, final solutions always
// have soft0 == 0, so feasibility coincides with the Java hard level.
constexpr std::int64_t kMissingSkillUnit = 1000000000LL;
constexpr std::int64_t kMakespanUnit = 10000000LL;

struct Score
{
    std::int64_t hard = 0;   // hard[0]: missing skills
    std::int64_t soft0 = 0;  // soft[0]: unassigned tasks
    std::int64_t soft1 = 0;  // soft[1]: sum of employee endTime^2
    std::int64_t soft2 = 0;  // soft[2]: priority-weighted task end times

    bool isFeasible() const { return hard >= 0; }

    bool operator<(const Score &o) const
    {
        if (hard != o.hard)
            return hard < o.hard;
        if (soft0 != o.soft0)
            return soft0 < o.soft0;
        if (soft1 != o.soft1)
            return soft1 < o.soft1;
        return soft2 < o.soft2;
    }
    bool operator>(const Score &o) const { return o < *this; }
    bool operator<=(const Score &o) const { return !(o < *this); }
    bool operator>=(const Score &o) const { return !(*this < o); }
    bool operator==(const Score &o) const
    {
        return hard == o.hard && soft0 == o.soft0 && soft1 == o.soft1
               && soft2 == o.soft2;
    }

    // BendableScore.toString() style: [0]hard/[0/-3725/-77]soft
    std::string toString() const
    {
        return "[" + std::to_string(hard) + "]hard/[" + std::to_string(soft0)
               + "/" + std::to_string(soft1) + "/" + std::to_string(soft2)
               + "]soft";
    }
};

inline optimaru::HardSoftScore toEngineScore(const Score &s)
{
    return {s.hard * kMissingSkillUnit + s.soft0,
            static_cast<double>(s.soft1) * static_cast<double>(kMakespanUnit)
                + static_cast<double>(s.soft2)};
}

// Inverse of toEngineScore. Every constraint penalizes (all levels <= 0 and
// integral), so truncating integer division decodes the levels exactly.
inline Score fromEngineScore(const optimaru::HardSoftScore &e)
{
    const std::int64_t combinedSoft
        = static_cast<std::int64_t>(std::llround(e.soft));
    return {e.hard / kMissingSkillUnit,
            e.hard % kMissingSkillUnit,
            combinedSoft / kMakespanUnit,
            combinedSoft % kMakespanUnit};
}

// ---------------------------------------------------------------------------
// Enums  (org.acme.taskassigning.domain.Affinity / Priority)
// ---------------------------------------------------------------------------
// Affinity of an employee for a customer: better affinity means the task
// takes fewer minutes (baseDuration x multiplier).
enum class Affinity
{
    NONE,    // multiplier 4
    LOW,     // multiplier 3
    MEDIUM,  // multiplier 2
    HIGH     // multiplier 1
};

inline int durationMultiplier(Affinity affinity)
{
    switch (affinity)
    {
    case Affinity::HIGH:
        return 1;
    case Affinity::MEDIUM:
        return 2;
    case Affinity::LOW:
        return 3;
    case Affinity::NONE:
    default:
        return 4;
    }
}

enum class Priority
{
    MINOR,
    MAJOR,
    CRITICAL
};

// The soft[2] end-time weight of each priority (the Java constraints'
// x 1 / x 2 / x 4 factors).
inline std::int64_t priorityWeight(Priority priority)
{
    switch (priority)
    {
    case Priority::CRITICAL:
        return 4;
    case Priority::MAJOR:
        return 2;
    case Priority::MINOR:
    default:
        return 1;
    }
}

// ---------------------------------------------------------------------------
// Customer  (problem fact)
// ---------------------------------------------------------------------------
struct Customer
{
    std::string id;
    std::string name;
};

// ---------------------------------------------------------------------------
// TaskType  (problem fact)
// ---------------------------------------------------------------------------
struct TaskType
{
    std::string code;
    std::string title;
    Minutes baseDuration = 0;  // minutes
    std::vector<std::string> requiredSkills;
};

// ---------------------------------------------------------------------------
// Task  (@PlanningEntity; the LIST element)
// ---------------------------------------------------------------------------
// Problem facts are the immutable fields; the shadow fields (employee,
// startTime) are derived from the employees' task lists, exactly like the
// shadow variables in the Java Task (@InverseRelationShadowVariable /
// @PreviousElementShadowVariable / startTimeSupplier):
//   startTime = null                          if unassigned
//             = minStartTime                  if first in the list
//             = max(previous.endTime, minStartTime) otherwise
//   endTime   = startTime + baseDuration * affinityMultiplier
struct Task
{
    std::string id;
    int taskType = -1;  // index into TaskAssigningSolution::taskTypes
    int indexInTaskType = 0;
    int customer = -1;  // index into TaskAssigningSolution::customers
    Minutes minStartTime = 0;
    Priority priority = Priority::MINOR;

    // --- shadow (recomputed by ScoreCalculator::updateShadowVariables) ---
    int employee = -1;  // -1 == unassigned (employee == null)
    Minutes startTime = kNullTime;
};

// ---------------------------------------------------------------------------
// Employee  (@PlanningEntity holding the @PlanningListVariable)
// ---------------------------------------------------------------------------
// tasks is the @PlanningListVariable(allowsUnassignedValues = true): the
// ordered task indices this employee performs back to back.
struct Employee
{
    std::string id;
    std::string fullName;
    std::vector<std::string> skills;
    // Affinity per customer index; missing entries mean Affinity::NONE
    // (Employee.getAffinity's getOrDefault).
    std::vector<Affinity> affinityByCustomer;
    std::vector<int> tasks;  // ordered task indices — the planning variable

    Affinity affinity(int customerIndex) const
    {
        if (customerIndex < 0
            || customerIndex >= static_cast<int>(affinityByCustomer.size()))
            return Affinity::NONE;
        return affinityByCustomer[customerIndex];
    }
};

// ---------------------------------------------------------------------------
// TaskAssigningSolution  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct TaskAssigningSolution
{
    std::vector<TaskType> taskTypes;
    std::vector<Customer> customers;
    std::vector<Task> tasks;
    std::vector<Employee> employees;

    Score score;

    // Duration of `task` when done by `employeeIndex` (Task.getDuration):
    // baseDuration x the affinity multiplier; unassigned uses NONE (x 4).
    Minutes taskDuration(const Task &task, int employeeIndex) const
    {
        const Affinity affinity
            = employeeIndex < 0
                  ? Affinity::NONE
                  : employees[employeeIndex].affinity(task.customer);
        return taskTypes[task.taskType].baseDuration
               * durationMultiplier(affinity);
    }

    // Task.getMissingSkillCount: required skills of the task's type that the
    // employee does not have.
    int missingSkillCount(const Task &task, int employeeIndex) const
    {
        if (employeeIndex < 0)
            return 0;
        const Employee &employee = employees[employeeIndex];
        int count = 0;
        for (const std::string &skill : taskTypes[task.taskType].requiredSkills)
        {
            bool found = false;
            for (const std::string &owned : employee.skills)
            {
                if (owned == skill)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
                ++count;
        }
        return count;
    }
};

}  // namespace ta

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization  (the @PlanningEntity/@PlanningListVariable
// annotations of the C++ engine): the lists are the employees' ordered task
// index sequences, the elements are the tasks. A task missing from every list
// is unassigned (allowsUnassignedValues = true); the engine's construction
// heuristic assigns every task and local search keeps them assigned. The Java
// quickstart has no @PlanningPin, so no isPinned hook is declared.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<ta::TaskAssigningSolution>
{
    static constexpr optimaru::VariableKind kind = optimaru::VariableKind::LIST;
    static int listCount(const ta::TaskAssigningSolution &s)
    {
        return static_cast<int>(s.employees.size());
    }
    static int elementCount(const ta::TaskAssigningSolution &s)
    {
        return static_cast<int>(s.tasks.size());
    }
    static int listSize(const ta::TaskAssigningSolution &s, int listIndex)
    {
        return static_cast<int>(s.employees[listIndex].tasks.size());
    }
    static int
    getElement(const ta::TaskAssigningSolution &s, int listIndex, int position)
    {
        return s.employees[listIndex].tasks[position];
    }
    static void insertElement(ta::TaskAssigningSolution &s,
                              int listIndex,
                              int position,
                              int element)
    {
        std::vector<int> &tasks = s.employees[listIndex].tasks;
        tasks.insert(tasks.begin() + position, element);
    }
    static int
    removeElement(ta::TaskAssigningSolution &s, int listIndex, int position)
    {
        std::vector<int> &tasks = s.employees[listIndex].tasks;
        const int element = tasks[position];
        tasks.erase(tasks.begin() + position);
        return element;
    }
    static void reverseSegment(ta::TaskAssigningSolution &s,
                               int listIndex,
                               int first,
                               int last)
    {
        std::vector<int> &tasks = s.employees[listIndex].tasks;
        std::reverse(tasks.begin() + first, tasks.begin() + last + 1);
    }
};

#endif  // TA_DOMAIN_HPP

#ifndef TA_CONSTRAINTS_COMMON_HPP
#define TA_CONSTRAINTS_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <vector>

#include "ta/domain.hpp"
#include "ta/score_calculator.hpp"
#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace ta
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The six constraints of the reference task-assigning
// TaskAssigningConstraintProvider, split into one small component each on the
// engine's DecomposedScoreCalculator — an optional assembly path NEXT TO the
// monolithic TaskAssigningIncrementalScoreCalculator, which stays the tuned
// default fast path.
//
// Unit choice: with the LIST descriptor the calculator's unit is one LIST,
// i.e. one employee; members are that employee's task indices IN EXECUTION
// ORDER. Five of the six rules are per-employee sums over that ordered chain
// and therefore plain UnitConstraints. minimizeUnassignedTasks counts the
// tasks in NO list at all, which belongs to no unit, so it runs as a
// GlobalConstraint (the school-timetabling UnassignedLesson pattern).
//
// A component never reads the Task shadow variables (employee, startTime):
// walkChain below re-derives the start/end chain from the unit's own member
// order, which is what makes each cost a pure function of (solution, unit,
// members) as the engine's contract requires. The LIST path re-costs a
// touched list whole, so no delta contract is needed.
//
// Score levels: the quickstart scores with BendableScore(1 hard, 3 soft),
// which this domain folds into the engine's HardSoftScore as
//   engine.hard = hard[0] * kMissingSkillUnit + soft[0]
//   engine.soft = soft[1] * kMakespanUnit     + soft[2]
// (see domain.hpp). A component's default weight therefore carries its
// level's unit factor: noMissingSkills weighs kMissingSkillUnit on the hard
// level, minimizeUnassignedTasks weighs 1 on the same hard level,
// minimizeMakespan weighs kMakespanUnit on the soft level, and the three
// priority rules weigh their plain 4 / 2 / 1 factors on the soft level.
//
//   DecomposedCalculator calc;
//   defaultConstraints()              // the whole default set
//       .weight("...", 2)            // ... then only what differs
//       .soft("...")
//       .drop("...")
//       .into(calc);
//   solver.solve(solution, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// The level and the weight of each component are DEFAULTS — assembly-time
// metadata, not part of the cost. Override them per instance via the mutable
// `isHard` / `weight` / `exponent`.
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty; the calculator negates into the score. An assembly of all six
// components at their default weights therefore scores bit-for-bit like
// ScoreCalculator::calculateScore.

using DecomposedCalculator
    = optimaru::DecomposedScoreCalculator<TaskAssigningSolution>;
using ConstraintAssembly = optimaru::ConstraintAssembly<TaskAssigningSolution>;
using TaUnitConstraint = optimaru::UnitConstraint<TaskAssigningSolution>;
using TaGlobalConstraint = optimaru::GlobalConstraint<TaskAssigningSolution>;

namespace detail
{

// Walks one employee's task chain in execution order, calling
// `onTaskEnd(taskIndex, endTime)` for each task, and returns the employee's
// end time (0 for an empty list — Employee.getEndTime).
//
// This is Task.startTimeSupplier verbatim: the first task starts at its own
// minStartTime, every later one waits for the previous end. Re-deriving it
// here (instead of reading Task::startTime) keeps every component a pure
// function of the unit's members, so a partially updated shadow state can
// never leak into a score.
template <typename F>
inline Minutes walkChain(const TaskAssigningSolution &solution,
                         int employeeIndex,
                         const std::vector<int> &members,
                         F &&onTaskEnd)
{
    Minutes previousEnd = 0;
    bool first = true;
    for (int taskIndex : members)
    {
        const Task &task = solution.tasks[taskIndex];
        const Minutes startTime
            = first ? task.minStartTime
                    : std::max(previousEnd, task.minStartTime);
        previousEnd = startTime + solution.taskDuration(task, employeeIndex);
        first = false;
        onTaskEnd(taskIndex, previousEnd);
    }
    return previousEnd;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Priority end-time family
// ---------------------------------------------------------------------------
// Shared base of the three soft[2] rules (critical x 4 / major x 2 / minor
// x 1): the same per-employee chain walk, restricted to the tasks of one
// priority. The x 4 / x 2 / x 1 factors are the components' default weights,
// so re-pricing one priority is a weight override like any other.
struct PriorityTaskEndTime : TaUnitConstraint
{
    PriorityTaskEndTime(std::string constraintName,
                        double weight,
                        bool hard,
                        Priority priority)
        : TaUnitConstraint(std::move(constraintName), weight, hard),
          priority_(priority)
    {
    }

    double cost(const TaskAssigningSolution &solution,
                int unit,
                const std::vector<int> &members) const override
    {
        std::int64_t total = 0;
        const Priority priority = priority_;
        detail::walkChain(solution,
                          unit,
                          members,
                          [&](int taskIndex, Minutes endTime)
                          {
                              if (solution.tasks[taskIndex].priority
                                  == priority)
                                  total += endTime;
                          });
        return static_cast<double>(total);
    }

private:
    Priority priority_;
};

}  // namespace ta

#endif  // TA_CONSTRAINTS_COMMON_HPP

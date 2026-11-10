#ifndef TA_CONSTRAINTS_COMMON_HPP
#define TA_CONSTRAINTS_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <vector>

#include "ta/domain.hpp"
#include "ta/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace ta
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The six constraints of this use case, split into one small component each
// on the engine's DecomposedObjective — an optional assembly path NEXT TO the
// monolithic TaskAssigningIncrementalScoreCalculator, which stays the tuned
// default fast path.
//
// Unit choice: with the LIST descriptor the calculator's unit is one LIST,
// i.e. one employee; members are that employee's task indices IN EXECUTION
// ORDER. Five of the six rules are per-employee sums over that ordered chain
// and therefore plain UnitConstraints. unassignedTasksCost counts the tasks
// in NO list at all, which belongs to no unit, so it runs as a
// GlobalConstraint (the school-timetabling UnassignedLessonsCost pattern).
//
// A component never reads the derived Task fields (employee, startTime):
// walkChain below re-derives the start/end chain from the unit's own member
// order, which is what makes each cost a pure function of (solution, unit,
// members) as the engine's contract requires. The LIST path re-costs a
// touched list whole, so no delta contract is needed.
//
// Score levels: this use case scores four Bendable levels (1 hard, 3 soft).
// The components score on the engine's three levels and the domain folds
// them into Score (domain.hpp, kMediumFold and toEngineScore):
//   hard[0] requiredSkillCost         Level::Hard,   weight 1
//   soft[0] unassignedTasksCost       Level::Medium, weight 1
//   soft[1] squaredMakespanCost       Level::Soft,   weight kMakespanUnit (10^7)
//   soft[2] {critical|major|minor}PriorityTaskEndTimeCost
//                                     Level::Soft,   weight 4 / 2 / 1
//   engine.hard = hard * kMissingSkillUnit + medium   (MediumFold::below_hard)
//   engine.soft = soft                                (= soft1 * 10^7 + soft2)
// Every weight is in units of its level; the 10^9 hard unit lives in the
// fold, not in requiredSkillCost's weight. soft[1] and soft[2] share the one
// soft level, so squaredMakespanCost's default weight 10^7 is what keeps
// soft[1] lexicographically above soft[2] (the Score adapter decodes that
// packing).
//
//   DecomposedCalculator calc;
//   defaultConstraints()                                    // the whole default set
//       .weight("criticalPriorityTaskEndTimeCost", 2)       // ... then only what differs
//       .soft("requiredSkillCost")
//       .drop("majorPriorityTaskEndTimeCost")
//       .into(calc);
//   solver.solve(solution, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// Each component declares its defaults once, as static constexpr
// kDefaultWeight / kDefaultLevel passed to the base constructor; they are
// assembly-time metadata, not part of the cost. Override them per instance
// with the constructor's weight argument or the mutable `level` / `weight` /
// `exponent`, or by name through ConstraintAssembly. The monolithic
// calculators read the same kDefaultWeight constants, which is why the
// default assembly scores bit-for-bit like them.
//
// Registry names are the struct names with a lower-case first letter
// ("requiredSkillCost"); Python sees them through python_name()
// ("required_skill_cost").
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty, 0 = no violation; the calculator negates into the score.

using DecomposedCalculator
    = optimaru::DecomposedObjective<TaskAssigningSolution>;
using ConstraintAssembly = optimaru::ConstraintAssembly<TaskAssigningSolution>;
using TaUnitConstraint = optimaru::UnitConstraint<TaskAssigningSolution>;
using TaGlobalConstraint = optimaru::GlobalConstraint<TaskAssigningSolution>;

namespace detail
{

// Walks one employee's task chain in execution order, calling
// `onTaskEnd(taskIndex, endTime)` for each task, and returns the employee's
// end time (0 for an empty list).
//
// The first task starts at its own
// minStartTime, every later one waits for the previous end. Re-deriving it
// here (instead of reading Task::startTime) keeps every component a pure
// function of the unit's members, so a partially updated derived state can
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
// priority. The x 4 / x 2 / x 1 factors are the components' kDefaultWeight,
// so re-pricing one priority is a weight override like any other. Raw cost
// per employee = the summed end times (minutes) of its tasks of that
// priority; 0 = no task of that priority on the list.
struct PriorityTaskEndTimeCostBase : TaUnitConstraint
{
    PriorityTaskEndTimeCostBase(std::string constraint_name,
                                double weight,
                                optimaru::Level level,
                                Priority priority)
        : TaUnitConstraint(std::move(constraint_name), weight, level),
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

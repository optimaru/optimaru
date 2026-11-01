"""Class-based CP-SAT plugins for task assigning.

Each plugin subclasses the domain spec of the same name and only builds its
expression; the default level and weight come from the engine's
``task_assigning.constraint_info()`` (cpsat-common reads the row), so the two
solvers cannot disagree on a default. The levels are the engine's three:
``required_skill_cost`` hard, ``unassigned_tasks_cost`` medium, the other four
soft. The Bendable soft[1]-over-soft[2] order is carried by
``squared_makespan_cost``'s default weight 10^7 on the one soft level, as in the
engine.

Offsets and repeats: every plugin adds one term per unit the engine evaluates
(``terms_per_unit``): one per employee (an idle one included) for the unit
constraints, one for the global ``unassigned_tasks_cost``'s total. The priority
end-time plugins add one term per task at offset 0 (a linear term sums to the same
per-employee raw cost, so the default model is unchanged) and one per employee under
an offset. A constraint picked several times adds one set of terms per pick.
"""

from dataclasses import dataclass
from typing import Any, ClassVar

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from ortools.sat.python import cp_model
from task_assigning import constraints as domain
from task_assigning.models import TaskAssigningSolutionModel


@dataclass(frozen=True)
class TaskContext:
    model: cp_model.CpModel
    problem: TaskAssigningSolutionModel
    task_type: dict[str, Any]
    duration: dict[tuple[int, int], int]
    horizon: int
    assigned: dict[tuple[int, int], Any]
    unassigned: list[Any]
    end: list[Any]
    contributions: list[list[Any]]


class RequiredSkillCost(domain.RequiredSkillCost, ConstraintPlugin[TaskContext]):
    terms_per_unit = True  # one term per employee

    def build(self, context: TaskContext, objective: ObjectiveBuilder) -> None:
        for employee_index, employee in enumerate(context.problem.employees):
            skills = set(employee.skills)
            missing = [
                len(
                    set(context.task_type[task.task_type_code].required_skills) - skills
                )
                for task in context.problem.tasks
            ]
            objective.add(
                self.name,
                sum(
                    count * context.assigned[employee_index, task_index]
                    for task_index, count in enumerate(missing)
                ),
                bound=sum(missing),
            )


class UnassignedTasksCost(domain.UnassignedTasksCost, ConstraintPlugin[TaskContext]):
    terms_per_unit = True  # a global constraint: one term for its total

    def build(self, context: TaskContext, objective: ObjectiveBuilder) -> None:
        objective.add(self.name, sum(context.unassigned), bound=len(context.unassigned))


class SquaredMakespanCost(domain.SquaredMakespanCost, ConstraintPlugin[TaskContext]):
    terms_per_unit = True  # one term per employee, an idle one included

    def build(self, context: TaskContext, objective: ObjectiveBuilder) -> None:
        for employee, contributions in enumerate(context.contributions):
            employee_end = context.model.new_int_var(
                0, context.horizon, f"employee_{employee}_end"
            )
            context.model.add_max_equality(employee_end, contributions or [0])
            square = context.model.new_int_var(
                0,
                context.horizon * context.horizon,
                f"employee_{employee}_end_square",
            )
            context.model.add_multiplication_equality(
                square, (employee_end, employee_end)
            )
            objective.add(self.name, square, bound=context.horizon * context.horizon)


class _PriorityEndPlugin(ConstraintPlugin[TaskContext]):
    priority: ClassVar[str]
    terms_per_unit = True  # per task at offset 0 (linear), per employee under an offset

    def build(self, context: TaskContext, objective: ObjectiveBuilder) -> None:
        _spec, config = objective.selected[self.name]
        if config.offset != 0:
            # The engine's unit is the employee: shape each one's summed end times.
            mine = [
                task_index
                for task_index, task in enumerate(context.problem.tasks)
                if task.priority == self.priority
            ]
            for contributions in context.contributions:
                objective.add(
                    self.name,
                    sum(contributions[task_index] for task_index in mine),
                    bound=context.horizon * len(mine),
                )
            return
        for task_index, task in enumerate(context.problem.tasks):
            if task.priority != self.priority:
                continue
            active_end = context.model.new_int_var(
                0, context.horizon, f"task_{task_index}_active_end"
            )
            context.model.add(active_end == context.end[task_index]).only_enforce_if(
                context.unassigned[task_index].Not()
            )
            context.model.add(active_end == 0).only_enforce_if(
                context.unassigned[task_index]
            )
            objective.add(self.name, active_end, bound=context.horizon)


class CriticalPriorityTaskEndTimeCost(
    domain.CriticalPriorityTaskEndTimeCost, _PriorityEndPlugin
):
    priority = "CRITICAL"


class MajorPriorityTaskEndTimeCost(
    domain.MajorPriorityTaskEndTimeCost, _PriorityEndPlugin
):
    priority = "MAJOR"


class MinorPriorityTaskEndTimeCost(
    domain.MinorPriorityTaskEndTimeCost, _PriorityEndPlugin
):
    priority = "MINOR"


# Plugin order = task_assigning.list_constraints() order (the catalog order).
REGISTRY = ConstraintRegistry(
    [
        RequiredSkillCost(),
        SquaredMakespanCost(),
        CriticalPriorityTaskEndTimeCost(),
        MajorPriorityTaskEndTimeCost(),
        MinorPriorityTaskEndTimeCost(),
        UnassignedTasksCost(),
    ]
)

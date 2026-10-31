"""Class-based CP-SAT plugins for task assigning.

Each plugin subclasses the domain spec of the same name and only builds its
expression; the default level and weight come from the engine's
``task_assigning.constraint_info()`` (cpsat-common reads the row), so the two
solvers cannot disagree on a default. The levels are the engine's three:
``required_skill_cost`` hard, ``unassigned_tasks_cost`` medium, the other four
soft. The Bendable soft[1]-over-soft[2] order is carried by
``squared_makespan_cost``'s default weight 10^7 on the one soft level, as in the
engine.
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
    def build(self, context: TaskContext, objective: ObjectiveBuilder) -> None:
        for task_index, task in enumerate(context.problem.tasks):
            required = set(context.task_type[task.task_type_code].required_skills)
            for employee_index, employee in enumerate(context.problem.employees):
                missing = len(required - set(employee.skills))
                literal = context.assigned[employee_index, task_index]
                objective.add(self.name, missing * literal, bound=missing)


class UnassignedTasksCost(domain.UnassignedTasksCost, ConstraintPlugin[TaskContext]):
    def build(self, context: TaskContext, objective: ObjectiveBuilder) -> None:
        for literal in context.unassigned:
            objective.add(self.name, literal, bound=1)


class SquaredMakespanCost(domain.SquaredMakespanCost, ConstraintPlugin[TaskContext]):
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

    def build(self, context: TaskContext, objective: ObjectiveBuilder) -> None:
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

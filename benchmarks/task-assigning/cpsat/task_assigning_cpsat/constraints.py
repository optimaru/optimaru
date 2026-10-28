"""Class-based CP-SAT plugins for task assigning."""

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


class NoMissingSkills(domain.NoMissingSkills, ConstraintPlugin[TaskContext]):
    default_level = "hard"

    def build(self, context: TaskContext, objective: ObjectiveBuilder) -> None:
        for task_index, task in enumerate(context.problem.tasks):
            required = set(context.task_type[task.task_type_code].required_skills)
            for employee_index, employee in enumerate(context.problem.employees):
                missing = len(required - set(employee.skills))
                literal = context.assigned[employee_index, task_index]
                objective.add(self.name, missing * literal, bound=missing)


class MinimizeUnassignedTasks(
    domain.MinimizeUnassignedTasks, ConstraintPlugin[TaskContext]
):
    default_level = "soft0"

    def build(self, context: TaskContext, objective: ObjectiveBuilder) -> None:
        for literal in context.unassigned:
            objective.add(self.name, literal, bound=1)


class MinimizeMakespan(domain.MinimizeMakespan, ConstraintPlugin[TaskContext]):
    default_level = "soft1"

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


class CriticalPriorityTaskEndTime(
    domain.CriticalPriorityTaskEndTime, _PriorityEndPlugin
):
    default_level = "soft2"
    default_weight = 4
    priority = "CRITICAL"


class MajorPriorityTaskEndTime(domain.MajorPriorityTaskEndTime, _PriorityEndPlugin):
    default_level = "soft2"
    default_weight = 2
    priority = "MAJOR"


class MinorPriorityTaskEndTime(domain.MinorPriorityTaskEndTime, _PriorityEndPlugin):
    default_level = "soft2"
    priority = "MINOR"


REGISTRY = ConstraintRegistry(
    [
        NoMissingSkills(),
        MinimizeUnassignedTasks(),
        MinimizeMakespan(),
        CriticalPriorityTaskEndTime(),
        MajorPriorityTaskEndTime(),
        MinorPriorityTaskEndTime(),
    ]
)

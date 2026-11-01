"""Class-based CP-SAT plugins for project/job scheduling.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``project_job.constraint_info()`` (the C++
components' kDefaultLevel / kDefaultWeight), so they are not restated here.
The medium constraint (``project_delay_cost``) lands on the CP-SAT medium level
at weight 1, in medium units, exactly like the engine (whose domain folds
medium into soft at 10^9); ``level="soft"`` demotes it to soft on both paths.
"""

from dataclasses import dataclass
from typing import Any

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from ortools.sat.python import cp_model
from project_job import constraints as domain
from project_job.models import ProjectJobScheduleModel


@dataclass(frozen=True)
class ProjectContext:
    model: cp_model.CpModel
    problem: ProjectJobScheduleModel
    intervals_by_resource: dict[str, list[tuple[Any, int]]]
    nonrenewable_terms: dict[str, list[Any]]
    project_delay: list[Any]
    makespan: Any
    horizon: int


class RenewableResourceCapacityCost(
    domain.RenewableResourceCapacityCost, ConstraintPlugin[ProjectContext]
):
    def build(self, context: ProjectContext, objective: ObjectiveBuilder) -> None:
        for resource in context.problem.resources:
            if not resource.renewable:
                continue
            entries = context.intervals_by_resource[resource.id]
            total_demand = sum(entry[1] for entry in entries)
            for time in range(context.horizon):
                active_demands = []
                for index, (_interval, demand, start, end, present) in enumerate(
                    entries
                ):
                    started = context.model.new_bool_var(
                        f"{resource.id}_{index}_started_{time}"
                    )
                    unfinished = context.model.new_bool_var(
                        f"{resource.id}_{index}_unfinished_{time}"
                    )
                    context.model.add(start <= time).only_enforce_if(started)
                    context.model.add(start >= time + 1).only_enforce_if(
                        started.negated()
                    )
                    context.model.add(end >= time + 1).only_enforce_if(unfinished)
                    context.model.add(end <= time).only_enforce_if(unfinished.negated())
                    active = context.model.new_bool_var(
                        f"{resource.id}_{index}_active_{time}"
                    )
                    context.model.add_bool_and(
                        [present, started, unfinished]
                    ).only_enforce_if(active)
                    context.model.add_bool_or(
                        [present.negated(), started.negated(), unfinished.negated()]
                    ).only_enforce_if(active.negated())
                    active_demands.append(demand * active)
                excess = context.model.new_int_var(
                    0, total_demand, f"{resource.id}_excess_{time}"
                )
                context.model.add_max_equality(
                    excess, [0, sum(active_demands) - resource.capacity]
                )
                objective.add(self.name, excess, bound=total_demand)


class NonRenewableResourceCapacityCost(
    domain.NonRenewableResourceCapacityCost, ConstraintPlugin[ProjectContext]
):
    def build(self, context: ProjectContext, objective: ObjectiveBuilder) -> None:
        for resource in context.problem.resources:
            if not resource.renewable:
                terms = context.nonrenewable_terms[resource.id]
                total = sum(
                    requirement.requirement
                    for job in context.problem.jobs
                    for mode in job.execution_modes
                    for requirement in mode.resource_requirements
                    if requirement.resource_id == resource.id
                )
                excess = context.model.new_int_var(
                    0, total, f"{resource.id}_nonrenewable_excess"
                )
                context.model.add_max_equality(
                    excess, [0, sum(terms) - resource.capacity]
                )
                objective.add(self.name, excess, bound=total)


class ProjectDelayCost(domain.ProjectDelayCost, ConstraintPlugin[ProjectContext]):
    def build(self, context: ProjectContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(context.project_delay),
            bound=len(context.project_delay) * context.horizon,
        )


class MakespanCost(domain.MakespanCost, ConstraintPlugin[ProjectContext]):
    def build(self, context: ProjectContext, objective: ObjectiveBuilder) -> None:
        objective.add(self.name, context.makespan, bound=context.horizon)


# Plugin order = project_job.list_constraints() order, so catalog() lists the
# engine's catalog.
REGISTRY = ConstraintRegistry(
    [
        NonRenewableResourceCapacityCost(),
        RenewableResourceCapacityCost(),
        ProjectDelayCost(),
        MakespanCost(),
    ]
)

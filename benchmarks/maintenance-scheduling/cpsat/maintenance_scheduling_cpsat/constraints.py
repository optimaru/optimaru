"""Class-based CP-SAT plugins for maintenance scheduling.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``maintenance_scheduling.constraint_info()`` (the
C++ components' kDefaultLevel / kDefaultWeight — e.g. after_ideal_end_date_cost
soft 1'000'000 and tag_conflict_cost soft 1'000), so they are not restated here.
The registry lists the plugins in ``list_constraints()`` order.

Every plugin adds one term per engine unit: the four date rules one per
(crew, start date) cell, every cell included (an empty one is the constant 0),
the two global conflict rules one for their total. So a pick's ``offset`` /
``clip`` shape each cell (or the total) exactly as the engine does
(``terms_per_unit``), and a constraint picked several times adds one set of
terms per pick.
"""

from dataclasses import dataclass, field
from datetime import date
from typing import Any, ClassVar

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from maintenance_scheduling import constraints as domain
from maintenance_scheduling.models import MaintenanceScheduleModel
from ortools.sat.python import cp_model


@dataclass
class MaintenanceContext:
    model: cp_model.CpModel
    problem: MaintenanceScheduleModel
    dates: list[date]
    options: list[tuple[int, int]]
    x: dict[tuple[int, int, int], Any]
    ends: list[list[date]]
    _and_cache: dict[tuple[int, ...], Any] = field(default_factory=dict)

    def both(self, key: tuple[int, ...], left: Any, right: Any) -> Any:
        if key not in self._and_cache:
            value = self.model.new_bool_var("pair_" + "_".join(map(str, key)))
            self.model.add(value <= left)
            self.model.add(value <= right)
            self.model.add(value >= left + right - 1)
            self._and_cache[key] = value
        return self._and_cache[key]


class _DateBoundaryPlugin(ConstraintPlugin[MaintenanceContext]):
    boundary: ClassVar[str]
    terms_per_unit = True  # one term per (crew, start date) cell

    def raw(self, job: Any, start: date, end: date) -> int:
        if self.boundary == "min_start":
            return (
                max(0, (job.min_start_date - start).days) if job.min_start_date else 0
            )
        if self.boundary == "max_end":
            return max(0, (end - job.max_end_date).days) if job.max_end_date else 0
        if self.boundary == "before_ideal":
            return max(0, (job.ideal_end_date - end).days) if job.ideal_end_date else 0
        return max(0, (end - job.ideal_end_date).days) if job.ideal_end_date else 0

    def build(self, context: MaintenanceContext, objective: ObjectiveBuilder) -> None:
        jobs = context.problem.jobs
        for crew, day in context.options:
            amounts = [
                (
                    job_index,
                    self.raw(job, context.dates[day], context.ends[job_index][day]),
                )
                for job_index, job in enumerate(jobs)
            ]
            amounts = [(job_index, amount) for job_index, amount in amounts if amount]
            objective.add(
                self.name,
                sum(
                    amount * context.x[job_index, crew, day]
                    for job_index, amount in amounts
                ),
                bound=sum(amount for _, amount in amounts),
            )


class MinStartDateCost(domain.MinStartDateCost, _DateBoundaryPlugin):
    boundary = "min_start"


class MaxEndDateCost(domain.MaxEndDateCost, _DateBoundaryPlugin):
    boundary = "max_end"


class BeforeIdealEndDateCost(domain.BeforeIdealEndDateCost, _DateBoundaryPlugin):
    boundary = "before_ideal"


class AfterIdealEndDateCost(domain.AfterIdealEndDateCost, _DateBoundaryPlugin):
    boundary = "after_ideal"


class _PairPlugin(ConstraintPlugin[MaintenanceContext]):
    same_crew_only: ClassVar[bool]
    terms_per_unit = True  # a global constraint: one term for its total

    def build(self, context: MaintenanceContext, objective: ObjectiveBuilder) -> None:
        terms: list[Any] = []
        bound = 0
        for left in range(len(context.problem.jobs)):
            for right in range(left + 1, len(context.problem.jobs)):
                shared_tags = len(
                    set(context.problem.jobs[left].tags)
                    & set(context.problem.jobs[right].tags)
                )
                if not self.same_crew_only and not shared_tags:
                    continue
                for crew_left, day_left in context.options:
                    start_left = context.dates[day_left]
                    end_left = context.ends[left][day_left]
                    for crew_right, day_right in context.options:
                        if self.same_crew_only and crew_left != crew_right:
                            continue
                        start_right = context.dates[day_right]
                        end_right = context.ends[right][day_right]
                        overlap = max(
                            0,
                            (
                                min(end_left, end_right) - max(start_left, start_right)
                            ).days,
                        )
                        if not overlap:
                            continue
                        amount = (
                            overlap if self.same_crew_only else shared_tags * overlap
                        )
                        both = context.both(
                            (
                                left,
                                right,
                                crew_left,
                                day_left,
                                crew_right,
                                day_right,
                            ),
                            context.x[left, crew_left, day_left],
                            context.x[right, crew_right, day_right],
                        )
                        terms.append(amount * both)
                        bound += amount
        objective.add(self.name, sum(terms), bound=bound)


class CrewConflictCost(domain.CrewConflictCost, _PairPlugin):
    same_crew_only = True


class TagConflictCost(domain.TagConflictCost, _PairPlugin):
    same_crew_only = False


REGISTRY = ConstraintRegistry(
    [
        MinStartDateCost(),
        MaxEndDateCost(),
        BeforeIdealEndDateCost(),
        AfterIdealEndDateCost(),
        CrewConflictCost(),
        TagConflictCost(),
    ]
)

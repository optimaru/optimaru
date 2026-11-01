"""Class-based CP-SAT plugins for food packaging.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``food_packaging.constraint_info()`` (the C++
components' kDefaultLevel / kDefaultWeight), so they are not restated here.
The two medium constraints (``after_ideal_end_time_cost``,
``unassigned_jobs_cost``) land on the CP-SAT medium level at weight 1, in medium
units, exactly like the engine (whose domain folds medium into soft at 10^10);
``level="soft"`` demotes them to soft on both paths.

Offsets and repeats: every plugin adds one term per unit the engine evaluates
(``terms_per_unit``), so a pick's ``offset`` / ``clip`` shape each unit exactly as
the engine does, and a constraint picked several times adds one set of terms per
pick. The engine's per-line constraints evaluate the packaging lines (never the
unassigned pool); ``SquaredMakespanCost`` adds one term per line, the two
end-time constraints one per job at offset 0 (a linear term sums to the same
per-line raw cost) and one per line under an offset, and the two globals one for
their total.
"""

from dataclasses import dataclass
from typing import Any, ClassVar

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from food_packaging import constraints as domain
from food_packaging.models import PackagingScheduleModel, datetime_to_minute
from ortools.sat.python import cp_model


@dataclass(frozen=True)
class PackagingContext:
    model: cp_model.CpModel
    problem: PackagingScheduleModel
    origin: int
    horizon: int
    duration: list[int]
    unassigned: list[Any]
    end: list[Any]
    assigned: dict[tuple[int, int], Any]
    line_busy: list[Any]
    cleaning_windows_by_operator: dict[int, list[tuple[Any, Any, Any]]]


class OperatorCleaningConflictCost(
    domain.OperatorCleaningConflictCost, ConstraintPlugin[PackagingContext]
):
    terms_per_unit = True  # a global constraint: one term for its total

    def build(self, context: PackagingContext, objective: ObjectiveBuilder) -> None:
        overlaps = []
        for operator, windows in context.cleaning_windows_by_operator.items():
            for left in range(len(windows)):
                left_start, left_end, left_present = windows[left]
                for right in range(left + 1, len(windows)):
                    right_start, right_end, right_present = windows[right]
                    both = context.model.new_bool_var(
                        f"operator_{operator}_cleaning_pair_{left}_{right}"
                    )
                    context.model.add(both <= left_present)
                    context.model.add(both <= right_present)
                    context.model.add(both >= left_present + right_present - 1)
                    latest_start = context.model.new_int_var(
                        0,
                        context.horizon,
                        f"operator_{operator}_cleaning_pair_{left}_{right}_start",
                    )
                    earliest_end = context.model.new_int_var(
                        0,
                        context.horizon,
                        f"operator_{operator}_cleaning_pair_{left}_{right}_end",
                    )
                    context.model.add_max_equality(
                        latest_start, [left_start, right_start]
                    )
                    context.model.add_min_equality(earliest_end, [left_end, right_end])
                    raw_overlap = context.model.new_int_var(
                        0,
                        context.horizon,
                        f"operator_{operator}_cleaning_pair_{left}_{right}_raw_overlap",
                    )
                    context.model.add_max_equality(
                        raw_overlap, [0, earliest_end - latest_start]
                    )
                    overlap = context.model.new_int_var(
                        0,
                        context.horizon,
                        f"operator_{operator}_cleaning_pair_{left}_{right}_overlap",
                    )
                    context.model.add(overlap == raw_overlap).only_enforce_if(both)
                    context.model.add(overlap == 0).only_enforce_if(both.Not())
                    overlaps.append(overlap)
        objective.add(self.name, sum(overlaps), bound=context.horizon * len(overlaps))


class _EndTimePlugin(ConstraintPlugin[PackagingContext]):
    field: ClassVar[str]
    terms_per_unit = True  # per job at offset 0 (linear), per line under an offset

    def build(self, context: PackagingContext, objective: ObjectiveBuilder) -> None:
        _spec, config = objective.selected[self.name]
        lateness = []
        for job, job_data in enumerate(context.problem.jobs):
            limit = datetime_to_minute(getattr(job_data, self.field)) - context.origin
            raw_late = context.model.new_int_var(
                0, context.horizon, f"job_{job}_{self.field}_raw_late"
            )
            context.model.add_max_equality(raw_late, [0, context.end[job] - limit])
            late = context.model.new_int_var(
                0, context.horizon, f"job_{job}_{self.field}_late"
            )
            context.model.add(late == raw_late).only_enforce_if(
                context.unassigned[job].Not()
            )
            context.model.add(late == 0).only_enforce_if(context.unassigned[job])
            if config.offset == 0:
                objective.add(self.name, late, bound=context.horizon)
            lateness.append(late)
        if config.offset == 0:
            return
        # The engine's unit is the line: shape each line's summed lateness.
        for line in range(len(context.problem.lines)):
            on_line = []
            for job, late in enumerate(lateness):
                part = context.model.new_int_var(
                    0, context.horizon, f"line_{line}_job_{job}_{self.field}_late"
                )
                assigned = context.assigned[line, job]
                context.model.add(part == late).only_enforce_if(assigned)
                context.model.add(part == 0).only_enforce_if(assigned.Not())
                on_line.append(part)
            objective.add(self.name, sum(on_line), bound=context.horizon * len(on_line))


class MaxEndTimeCost(domain.MaxEndTimeCost, _EndTimePlugin):
    field = "max_end_time"


class AfterIdealEndTimeCost(domain.AfterIdealEndTimeCost, _EndTimePlugin):
    field = "ideal_end_time"


class UnassignedJobsCost(domain.UnassignedJobsCost, ConstraintPlugin[PackagingContext]):
    terms_per_unit = True  # a global constraint: one term for its total

    def build(self, context: PackagingContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(
                duration * context.unassigned[job]
                for job, duration in enumerate(context.duration)
            ),
            bound=sum(context.duration),
        )


class SquaredMakespanCost(
    domain.SquaredMakespanCost, ConstraintPlugin[PackagingContext]
):
    terms_per_unit = True  # one term per line, an empty one included

    def build(self, context: PackagingContext, objective: ObjectiveBuilder) -> None:
        for line, busy in enumerate(context.line_busy):
            squared = context.model.new_int_var(
                0, context.horizon**2, f"line_{line}_busy_squared"
            )
            context.model.add_multiplication_equality(squared, [busy, busy])
            objective.add(self.name, squared, bound=context.horizon**2)


# Plugin order = food_packaging.list_constraints() order (unit constraints
# first, then the globals), so catalog() lists the engine's catalog.
REGISTRY = ConstraintRegistry(
    [
        MaxEndTimeCost(),
        AfterIdealEndTimeCost(),
        SquaredMakespanCost(),
        OperatorCleaningConflictCost(),
        UnassignedJobsCost(),
    ]
)

"""Class-based CP-SAT plugins for assembly-line balancing.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``assembly_line_balancing.constraint_info()`` (the
C++ components' kDefaultLevel / kDefaultWeight), so they are not restated here.

Every encoding is exact — the term equals the engine's raw cost for ANY
assignment, not only at the optimum — so a pinned assignment scores like
``constraint_costs()`` (the parity tests rely on it), whatever the weight:

- per station / per task maxima use ``add_max_equality`` (not ``>=`` slacks);
- "station s is used" / "a member of group g is on s" are exact ORs;
- the workload range is ``max(loads) - min(loads of the used stations)``.

Raw costs are the engine's (0 = no violation). Every task is assigned (CP-SAT
places each task on exactly one station 0..n-1), so the engine's "an unassigned
task contributes nothing" never comes up here.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from assembly_line_balancing import constraints as domain
from assembly_line_balancing.models import AssemblyLineBalancingProblemModel
from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from ortools.sat.python import cp_model


@dataclass(frozen=True)
class LineContext:
    model: cp_model.CpModel
    problem: AssemblyLineBalancingProblemModel
    index: dict[str, int]  # task id -> task index
    assigned: dict[tuple[int, int], Any]  # (task, station) -> bool
    station_of: list[Any]  # task -> int var in 0..n-1
    used: list[Any]  # station -> bool, exactly "holds a task"
    loads: list[Any]  # station -> linear expression of the durations on it

    @property
    def count(self) -> int:
        return len(self.problem.tasks)

    @property
    def total_duration(self) -> int:
        return sum(task.duration for task in self.problem.tasks)


def _clamped(context: LineContext, expression, upper: int, name: str):
    """An int var equal to max(0, expression) (exact)."""
    value = context.model.new_int_var(0, max(0, upper), name)
    context.model.add_max_equality(value, [0, expression])
    return value


def _weight(plugin: ConstraintPlugin, objective: ObjectiveBuilder):
    spec, config = objective.selected[plugin.name]
    return spec.weight if config.weight is None else config.weight


class StationCapacityCost(domain.StationCapacityCost, ConstraintPlugin[LineContext]):
    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        total = context.total_duration
        overflow = [
            _clamped(context, load - context.problem.cycle_time, total, f"overflow_{station}")
            for station, load in enumerate(context.loads)
        ]
        objective.add(self.name, sum(overflow), bound=total)


class TaskPrecedenceCost(domain.TaskPrecedenceCost, ConstraintPlugin[LineContext]):
    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        model = context.model
        reversed_arcs = []
        for position, edge in enumerate(context.problem.precedences):
            before, after = context.index[edge.before], context.index[edge.after]
            if before == after:
                continue  # a self-arc never costs anything
            flag = model.new_bool_var(f"reversed_{position}")
            model.add(context.station_of[before] > context.station_of[after]).only_enforce_if(flag)
            model.add(context.station_of[before] <= context.station_of[after]).only_enforce_if(flag.Not())
            reversed_arcs.append(flag)
        objective.add(self.name, sum(reversed_arcs), bound=len(reversed_arcs))


class MaxStationsCost(domain.MaxStationsCost, ConstraintPlugin[LineContext]):
    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        limit = context.problem.max_stations
        if limit is None:
            return
        n = context.count
        beyond = [
            _clamped(context, station_of - limit + 1, n - limit, f"beyond_max_stations_{task}")
            for task, station_of in enumerate(context.station_of)
        ]
        objective.add(self.name, sum(beyond), bound=n * max(0, n - limit))


class TaskStationBoundsCost(domain.TaskStationBoundsCost, ConstraintPlugin[LineContext]):
    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        n = context.count
        terms = []
        bound = 0
        for task_index, task in enumerate(context.problem.tasks):
            station_of = context.station_of[task_index]
            if task.min_station > 0:
                terms.append(
                    _clamped(context, task.min_station - station_of, task.min_station, f"below_min_{task_index}")
                )
                bound += task.min_station
            if task.max_station is not None:
                upper = max(0, n - 1 - task.max_station)
                terms.append(_clamped(context, station_of - task.max_station, upper, f"above_max_{task_index}"))
                bound += upper
        objective.add(self.name, sum(terms), bound=bound)


class FixedTaskStationsCost(domain.FixedTaskStationsCost, ConstraintPlugin[LineContext]):
    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        missed = []
        for task_index, task in enumerate(context.problem.tasks):
            if task.fixed_station is None:
                continue
            if task.fixed_station < context.count:
                missed.append(1 - context.assigned[task_index, task.fixed_station])
            else:
                missed.append(1)  # its fixed station is outside 0..n-1
        objective.add(self.name, sum(missed), bound=len(missed))


class ForbiddenTaskStationsCost(domain.ForbiddenTaskStationsCost, ConstraintPlugin[LineContext]):
    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        # A station listed twice is one forbidden station; one outside 0..n-1 can never be hit.
        hits = [
            context.assigned[task_index, station]
            for task_index, task in enumerate(context.problem.tasks)
            for station in sorted(set(task.forbidden_stations))
            if 0 <= station < context.count
        ]
        objective.add(self.name, sum(hits), bound=len(hits))


class SameStationGroupsCost(domain.SameStationGroupsCost, ConstraintPlugin[LineContext]):
    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        # A group costs (distinct stations of its members) - 1; a member listed twice counts once.
        model = context.model
        terms = []
        bound = 0
        for group_index, group in enumerate(context.problem.same_station_groups):
            members = list(dict.fromkeys(context.index[task_id] for task_id in group.task_ids))
            if len(members) < 2:
                continue  # one distinct member never splits
            present = []
            for station in range(context.count):
                flag = model.new_bool_var(f"group_{group_index}_on_{station}")
                model.add_max_equality(flag, [context.assigned[task, station] for task in members])
                present.append(flag)
            terms.append(sum(present) - 1)
            bound += len(members) - 1
        objective.add(self.name, sum(terms), bound=bound)


class DifferentStationPairsCost(domain.DifferentStationPairsCost, ConstraintPlugin[LineContext]):
    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        model = context.model
        together = []
        for position, pair in enumerate(context.problem.different_station_pairs):
            first, second = context.index[pair.first], context.index[pair.second]
            if first == second:
                together.append(1)  # a self-pair always shares its station
                continue
            flag = model.new_bool_var(f"pair_{position}_together")
            model.add(context.station_of[first] == context.station_of[second]).only_enforce_if(flag)
            model.add(context.station_of[first] != context.station_of[second]).only_enforce_if(flag.Not())
            together.append(flag)
        objective.add(self.name, sum(together), bound=len(together))


class UsedStationsCost(domain.UsedStationsCost, ConstraintPlugin[LineContext]):
    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        objective.add(self.name, sum(context.used), bound=context.count)


class WorkloadImbalanceCost(domain.WorkloadImbalanceCost, ConstraintPlugin[LineContext]):
    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        if not _weight(self, objective) or not context.loads:
            return  # the opt-in default (weight 0) adds nothing
        model = context.model
        total = context.total_duration
        heaviest = model.new_int_var(0, total, "max_station_load")
        lightest = model.new_int_var(0, total, "min_used_station_load")
        model.add_max_equality(heaviest, context.loads)
        # An unused station counts as `total`, never below a used one (n >= 1: one is used).
        model.add_min_equality(
            lightest, [load + total * (1 - used) for load, used in zip(context.loads, context.used, strict=True)]
        )
        objective.add(self.name, heaviest - lightest, bound=total)


# Plugin order = list_constraints() order.
REGISTRY = ConstraintRegistry(
    [
        StationCapacityCost(),
        TaskPrecedenceCost(),
        MaxStationsCost(),
        TaskStationBoundsCost(),
        FixedTaskStationsCost(),
        ForbiddenTaskStationsCost(),
        SameStationGroupsCost(),
        DifferentStationPairsCost(),
        UsedStationsCost(),
        WorkloadImbalanceCost(),
    ]
)

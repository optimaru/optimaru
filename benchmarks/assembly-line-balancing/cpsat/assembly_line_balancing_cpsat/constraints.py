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

Every plugin adds one term per engine unit: the six per-station constraints one
per station (every station, a constant 0 included; ``max_stations_cost`` and
``forbidden_task_stations_cost`` exactly their scoped stations, as the engine's
``scopedUnits()``), the four globals one for their total. So a pick's
``offset`` / ``clip`` shape each station (or the total) exactly as the engine
does (``terms_per_unit``), and a constraint picked several times adds one set
of terms per pick.
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


def _per_station(name, context: LineContext, objective: ObjectiveBuilder, stations, amount_of) -> None:
    """One term per station of ``stations``: the summed constant ``amount_of(task, station)``
    of the tasks on it (the constant 0 when no task can cost there)."""
    for station in stations:
        amounts = [(task, amount_of(task, station)) for task in range(context.count)]
        amounts = [(task, amount) for task, amount in amounts if amount]
        objective.add(
            name,
            sum(amount * context.assigned[task, station] for task, amount in amounts),
            bound=sum(amount for _, amount in amounts),
        )


class StationCapacityCost(domain.StationCapacityCost, ConstraintPlugin[LineContext]):
    terms_per_unit = True  # one term per station

    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        upper = max(0, context.total_duration - context.problem.cycle_time)
        for station, load in enumerate(context.loads):
            overflow = _clamped(context, load - context.problem.cycle_time, upper, f"overflow_{station}")
            objective.add(self.name, overflow, bound=upper)


class TaskPrecedenceCost(domain.TaskPrecedenceCost, ConstraintPlugin[LineContext]):
    terms_per_unit = True  # a global constraint: one term for its total

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
    terms_per_unit = True  # one term per station from max_stations on (the engine's scope)

    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        limit = context.problem.max_stations
        if limit is None:
            return  # the engine scopes the constraint to no station
        # A task on station s >= limit costs s - limit + 1; the stations below are not evaluated.
        _per_station(
            self.name,
            context,
            objective,
            range(max(0, limit), context.count),
            lambda task, station: station - limit + 1,
        )


class TaskStationBoundsCost(domain.TaskStationBoundsCost, ConstraintPlugin[LineContext]):
    terms_per_unit = True  # one term per station

    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        tasks = context.problem.tasks

        def outside(task: int, station: int) -> int:
            data = tasks[task]
            below = max(0, data.min_station - station)
            above = max(0, station - data.max_station) if data.max_station is not None else 0
            return below + above

        _per_station(self.name, context, objective, range(context.count), outside)


class FixedTaskStationsCost(domain.FixedTaskStationsCost, ConstraintPlugin[LineContext]):
    terms_per_unit = True  # one term per station

    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        tasks = context.problem.tasks
        # A task with a fixed station costs 1 on every other station (all of them when the
        # fixed station is outside 0..n-1).
        _per_station(
            self.name,
            context,
            objective,
            range(context.count),
            lambda task, station: int(tasks[task].fixed_station is not None and tasks[task].fixed_station != station),
        )


class ForbiddenTaskStationsCost(domain.ForbiddenTaskStationsCost, ConstraintPlugin[LineContext]):
    terms_per_unit = True  # one term per station some task forbids (the engine's scope)

    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        # A station listed twice is one forbidden station; one outside 0..n-1 can never be hit.
        forbidden = [set(task.forbidden_stations) for task in context.problem.tasks]
        scope = sorted({station for stations in forbidden for station in stations if 0 <= station < context.count})
        _per_station(
            self.name,
            context,
            objective,
            scope,
            lambda task, station: int(station in forbidden[task]),
        )


class SameStationGroupsCost(domain.SameStationGroupsCost, ConstraintPlugin[LineContext]):
    terms_per_unit = True  # a global constraint: one term for its total

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
    terms_per_unit = True  # one term per station: the pairs sharing it

    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        model = context.model
        pairs = [
            (context.index[pair.first], context.index[pair.second]) for pair in context.problem.different_station_pairs
        ]
        for station in range(context.count):
            together = []
            for position, (first, second) in enumerate(pairs):
                if first == second:
                    together.append(context.assigned[first, station])  # a self-pair shares its station
                    continue
                flag = model.new_bool_var(f"pair_{position}_together_on_{station}")
                model.add_bool_and(
                    [context.assigned[first, station], context.assigned[second, station]]
                ).only_enforce_if(flag)
                model.add_bool_or(
                    [context.assigned[first, station].Not(), context.assigned[second, station].Not()]
                ).only_enforce_if(flag.Not())
                together.append(flag)
            objective.add(self.name, sum(together), bound=len(together))


class UsedStationsCost(domain.UsedStationsCost, ConstraintPlugin[LineContext]):
    terms_per_unit = True  # a global constraint: one term for its total

    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        objective.add(self.name, sum(context.used), bound=context.count)


class WorkloadImbalanceCost(domain.WorkloadImbalanceCost, ConstraintPlugin[LineContext]):
    terms_per_unit = True  # a global constraint: one term for its total

    def build(self, context: LineContext, objective: ObjectiveBuilder) -> None:
        if not _weight(self, objective):
            return  # the opt-in default (weight 0) adds nothing, whatever the offset
        if not context.loads:
            objective.add(self.name, 0, bound=0)  # no station: the total is 0
            return
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

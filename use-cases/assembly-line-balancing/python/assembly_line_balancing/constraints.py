"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`alb.solve(problem)`) scores every default constraint at its
default weight. This module is the alternative path next to it: pick
constraints, change weights, flip hard/soft, and pass the list to
`solve(problem, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import assembly_line_balancing as alb
    from assembly_line_balancing.constraints import WorkloadImbalanceCost, get_constraints

    alb.solve(problem, constraints=get_constraints())                       # defaults as-is
    alb.solve(problem, constraints=[*get_constraints()[:-1], WorkloadImbalanceCost(weight=1)])

Default instances (`station_capacity_cost`, `task_precedence_cost`, ...,
`workload_imbalance_cost`) live at module level, so you can also pass a list of
just a few. Only listed constraints are scored — leave one out and its term
disappears from the score.

Every spec takes ``(weight=None, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (1 for every constraint except the opt-in
               workload_imbalance_cost, 0). The problem carries no weights;
               ``alb.constraint_info()`` reports the defaults.
- ``hard``     ``True`` = hard, ``False`` = soft, ``None`` = the default level.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| ** exponent``,
               per station for the per-station constraints and on the single
               total for the global ones (task_precedence_cost,
               same_station_groups_cost, used_stations_cost,
               workload_imbalance_cost).

Names (`name`) are the C++ component names in snake_case
(``StationCapacityCost`` -> ``station_capacity_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`alb.list_constraints()`.
"""

from ._constraint import Constraint, Score, score_costs, select

__all__ = [
    "Constraint",
    "Score",
    "score",
    "StationCapacityCost",
    "TaskPrecedenceCost",
    "MaxStationsCost",
    "TaskStationBoundsCost",
    "FixedTaskStationsCost",
    "ForbiddenTaskStationsCost",
    "SameStationGroupsCost",
    "DifferentStationPairsCost",
    "UsedStationsCost",
    "WorkloadImbalanceCost",
    "station_capacity_cost",
    "task_precedence_cost",
    "max_stations_cost",
    "task_station_bounds_cost",
    "fixed_task_stations_cost",
    "forbidden_task_stations_cost",
    "same_station_groups_cost",
    "different_station_pairs_cost",
    "used_stations_cost",
    "workload_imbalance_cost",
    "get_constraints",
]


class StationCapacityCost(Constraint):
    """Processing time above the cycle time, per station. Default: hard, weight 1."""

    name = "station_capacity_cost"
    label_en = "Station capacity"
    label_ko = "공정 용량"
    description_en = (
        "Processing time a station carries beyond the cycle time, summed over the stations. "
        "One unit of cost is one time unit over the cycle; 0 means every station finishes within the cycle time."
    )
    description_ko = (
        "공정마다 배정된 작업 시간 가운데 사이클 타임을 넘는 시간을 모두 더합니다. "
        "초과 시간 1단위가 비용 1이며, 0이면 모든 공정이 사이클 타임 안에 끝납니다."
    )


class TaskPrecedenceCost(Constraint):
    """Precedence arcs assigned in reverse station order. Default: hard, weight 1."""

    name = "task_precedence_cost"
    label_en = "Task precedence"
    label_ko = "작업 선후 관계"
    description_en = (
        "Precedence arcs whose earlier task sits on a later station than the task that must follow it. "
        "Each such arc costs 1; 0 means the line respects every precedence."
    )
    description_ko = (
        "먼저 해야 하는 작업이 뒤따르는 작업보다 뒤 공정에 놓인 선후 관계를 셉니다. "
        "그런 관계 하나가 비용 1이며, 0이면 모든 선후 관계를 지킵니다."
    )


class MaxStationsCost(Constraint):
    """Tasks placed beyond the line's max_stations. Default: hard, weight 1."""

    name = "max_stations_cost"
    label_en = "Maximum stations"
    label_ko = "최대 공정 수"
    description_en = (
        "How far each task sits beyond the last station max_stations allows: a task on station s costs "
        "s - max_stations + 1 (stations count from 0). 0 means every task is within the allowed stations."
    )
    description_ko = (
        "max_stations가 허용하는 마지막 공정을 넘어선 만큼을 작업마다 더합니다. 공정 s(0부터 셉니다)에 놓인 "
        "작업은 s - max_stations + 1만큼 비용이 듭니다. 0이면 모든 작업이 허용된 공정 안에 있습니다."
    )


class TaskStationBoundsCost(Constraint):
    """Tasks outside their own [min_station, max_station]. Default: hard, weight 1."""

    name = "task_station_bounds_cost"
    label_en = "Task station bounds"
    label_ko = "작업별 공정 범위"
    description_en = (
        "How many stations each task sits below its min_station or above its max_station, summed. "
        "0 means every task is inside its own station range."
    )
    description_ko = (
        "작업마다 min_station보다 앞이나 max_station보다 뒤로 벗어난 공정 수를 모두 더합니다. "
        "0이면 모든 작업이 자기 공정 범위 안에 있습니다."
    )


class FixedTaskStationsCost(Constraint):
    """Tasks not on their fixed_station. Default: hard, weight 1."""

    name = "fixed_task_stations_cost"
    label_en = "Fixed task stations"
    label_ko = "고정 공정"
    description_en = (
        "Tasks with a fixed_station that are assigned to another station; each costs 1. "
        "0 means every fixed task is on its station."
    )
    description_ko = (
        "fixed_station이 정해진 작업이 다른 공정에 배정되면 작업마다 비용 1입니다. "
        "0이면 고정된 작업이 모두 제 공정에 있습니다."
    )


class ForbiddenTaskStationsCost(Constraint):
    """Tasks on one of their forbidden_stations. Default: hard, weight 1."""

    name = "forbidden_task_stations_cost"
    label_en = "Forbidden task stations"
    label_ko = "금지 공정"
    description_en = (
        "Tasks assigned to a station listed in their forbidden_stations; each costs 1. "
        "0 means no task is on a forbidden station."
    )
    description_ko = (
        "작업이 forbidden_stations에 적힌 공정에 배정되면 작업마다 비용 1입니다. "
        "0이면 금지된 공정에 놓인 작업이 없습니다."
    )


class SameStationGroupsCost(Constraint):
    """Same-station groups split over several stations. Default: hard, weight 1."""

    name = "same_station_groups_cost"
    label_en = "Same-station zoning"
    label_ko = "같은 공정 묶음"
    description_en = (
        "For every same-station group, the number of stations its tasks use beyond the first. "
        "0 means each group sits on one station."
    )
    description_ko = (
        "같은 공정에 있어야 하는 묶음마다 작업들이 쓰는 공정 수에서 1을 뺀 값을 더합니다. "
        "0이면 모든 묶음이 한 공정에 모여 있습니다."
    )


class DifferentStationPairsCost(Constraint):
    """Different-station pairs sharing a station. Default: hard, weight 1."""

    name = "different_station_pairs_cost"
    label_en = "Different-station zoning"
    label_ko = "다른 공정 짝"
    description_en = (
        "Pairs of tasks that must be on different stations but share one; each such pair costs 1. "
        "0 means every pair is separated."
    )
    description_ko = (
        "서로 다른 공정에 있어야 하는 두 작업이 같은 공정에 놓이면 짝마다 비용 1입니다. "
        "0이면 모든 짝이 떨어져 있습니다."
    )


class UsedStationsCost(Constraint):
    """The number of stations in use (the SALBP-1 objective). Default: soft, weight 1.

    A step function of "is this station used", so moving one task cannot
    cross it — that is why the domain carries a separate CloseStationMove."""

    name = "used_stations_cost"
    label_en = "Stations used"
    label_ko = "사용 공정 수"
    description_en = (
        "The number of stations that hold at least one task; each open station costs 1. 0 means no task is assigned."
    )
    description_ko = "작업이 하나 이상 배정된 공정의 수입니다. 공정 하나가 비용 1이며, 0이면 배정된 작업이 없습니다."


class WorkloadImbalanceCost(Constraint):
    """Load range of the used stations. Default: soft, weight 0 (opt-in)."""

    name = "workload_imbalance_cost"
    label_en = "Workload imbalance"
    label_ko = "작업량 불균형"
    description_en = (
        "The processing time of the most loaded used station minus that of the least loaded one. "
        "Off by default (weight 0); 0 means every used station carries the same load."
    )
    description_ko = (
        "사용 중인 공정 가운데 가장 많이 맡은 공정과 가장 적게 맡은 공정의 작업 시간 차이입니다. "
        "기본으로는 꺼져 있으며(가중치 0), 0이면 사용 중인 모든 공정의 작업량이 같습니다."
    )


#: Instances at the default weights (weight=None). Reference them directly when picking.
station_capacity_cost = StationCapacityCost()
task_precedence_cost = TaskPrecedenceCost()
max_stations_cost = MaxStationsCost()
task_station_bounds_cost = TaskStationBoundsCost()
fixed_task_stations_cost = FixedTaskStationsCost()
forbidden_task_stations_cost = ForbiddenTaskStationsCost()
same_station_groups_cost = SameStationGroupsCost()
different_station_pairs_cost = DifferentStationPairsCost()
used_stations_cost = UsedStationsCost()
workload_imbalance_cost = WorkloadImbalanceCost()

# The default set, in list_constraints() order.
_DEFAULTS = (
    station_capacity_cost,
    task_precedence_cost,
    max_stations_cost,
    task_station_bounds_cost,
    fixed_task_stations_cost,
    forbidden_task_stations_cost,
    same_station_groups_cost,
    different_station_pairs_cost,
    used_stations_cost,
    workload_imbalance_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, every default constraint.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([*get_constraints()[:-1], WorkloadImbalanceCost(weight=1)])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) and a constraint named twice raise ValueError.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `alb.constraint_costs(problem)` returns: per constraint, the
    **raw** cost of each station (one total for the global constraints), before
    weight, exponent and level rounding. Fetch it once and rescore with different
    weights without re-solving::

        raw = alb.constraint_costs(problem)
        score(raw)                                              # default set
        score(raw, [station_capacity_cost, WorkloadImbalanceCost(weight=1)])

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent)`` and hard
    contributions are **rounded per unit** before summing (the same order as
    the engine; rounding the total afterwards gives different values for
    fractional hard weights).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.
    """
    return score_costs(costs, get_constraints(constraints))

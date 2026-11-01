"""The 0.1.5 pure-Python assembly-line-balancing scorer and construction, frozen as a test oracle.

This is a verbatim transcription of what ``assembly_line_balancing`` computed
before the 0.2.0 C++ port (``__init__.py``: ``_loads`` / ``constraint_costs`` /
the construction at the top of ``solve``; ``common.py``: ``score_costs``), kept
only so the tests can prove that the C++ port computes the same raw costs and
the same default score. It imports nothing from the package: it works on the
problem as plain data (``AssemblyLineBalancingProblemModel.model_dump()`` or the
JSON a client sends). Never import it from the package itself.

Names are the 0.1.5 catalog names; ``RENAMED`` maps them to the 0.2.0 names.

Semantics kept as they were (the C++ port differs only where noted in the
package README):

- an unassigned task (station -1) is scored as if -1 were a station index
  (bounds cost ``min_station + 1``, an unmet fixed station costs 1, a pair
  with both tasks at -1 counts 1, an arc into a task at -1 counts as
  reversed); the C++ port follows the engine convention instead (an
  unassigned task contributes 0), so parity is asserted on complete
  assignments and, for partial ones, against :func:`restricted_to_assigned`;
- ``score_costs`` rounds the hard total once (the engine rounds per unit; the
  two agree for integral weights).
"""

from __future__ import annotations

import math
from collections import defaultdict
from collections.abc import Mapping, Sequence
from typing import Any

#: 0.1.5 catalog order (list_constraints()).
OLD_NAMES = (
    "station_capacity",
    "task_precedence",
    "station_count_limit",
    "task_station_bounds",
    "fixed_task_stations",
    "forbidden_task_stations",
    "same_station_groups",
    "different_station_pairs",
    "minimize_stations",
    "workload_balance",
)

#: 0.1.5 defaults: name -> (hard, weight).
OLD_DEFAULTS = {
    "station_capacity": (True, 1.0),
    "task_precedence": (True, 1.0),
    "station_count_limit": (True, 1.0),
    "task_station_bounds": (True, 1.0),
    "fixed_task_stations": (True, 1.0),
    "forbidden_task_stations": (True, 1.0),
    "same_station_groups": (True, 1.0),
    "different_station_pairs": (True, 1.0),
    "minimize_stations": (False, 1.0),
    "workload_balance": (False, 0.0),
}

#: 0.1.5 constraint name -> its 0.2.0 registry name.
RENAMED = {
    "station_capacity": "station_capacity_cost",
    "task_precedence": "task_precedence_cost",
    "station_count_limit": "max_stations_cost",
    "task_station_bounds": "task_station_bounds_cost",
    "fixed_task_stations": "fixed_task_stations_cost",
    "forbidden_task_stations": "forbidden_task_stations_cost",
    "same_station_groups": "same_station_groups_cost",
    "different_station_pairs": "different_station_pairs_cost",
    "minimize_stations": "used_stations_cost",
    "workload_balance": "workload_imbalance_cost",
}


def _tasks(problem: Mapping[str, Any]) -> list[dict[str, Any]]:
    tasks = []
    for task in problem["tasks"]:
        tasks.append(
            {
                "id": task["id"],
                "duration": task["duration"],
                "min_station": task.get("min_station", 0),
                "max_station": task.get("max_station"),
                "fixed_station": task.get("fixed_station"),
                "forbidden_stations": list(task.get("forbidden_stations") or []),
                "station": task.get("station", -1),
            }
        )
    return tasks


def loads(problem: Mapping[str, Any]) -> list[int]:
    """0.1.5 ``_loads``: one entry per station 0..max(station), empty stations included."""
    tasks = _tasks(problem)
    count = max((task["station"] for task in tasks), default=-1) + 1
    result = [0] * count
    for task in tasks:
        if task["station"] >= 0:
            result[task["station"]] += task["duration"]
    return result


def constraint_costs(problem: Mapping[str, Any]) -> dict[str, list[int]]:
    """0.1.5 ``constraint_costs``: {old name: list of raw costs} (per station / task / group / pair)."""
    tasks = _tasks(problem)
    cycle_time = problem["cycle_time"]
    max_stations = problem.get("max_stations")
    station_loads = loads(problem)
    station = {task["id"]: task["station"] for task in tasks}
    reversed_arcs = sum(station[edge["before"]] > station[edge["after"]] for edge in problem.get("precedences") or [])
    return {
        "station_capacity": [max(0, load - cycle_time) for load in station_loads],
        "task_precedence": [reversed_arcs],
        "station_count_limit": [
            0 if max_stations is None else max(0, task["station"] - max_stations + 1) for task in tasks
        ],
        "task_station_bounds": [
            max(0, task["min_station"] - task["station"])
            + (0 if task["max_station"] is None else max(0, task["station"] - task["max_station"]))
            for task in tasks
        ],
        "fixed_task_stations": [
            int(task["fixed_station"] is not None and task["station"] != task["fixed_station"]) for task in tasks
        ],
        "forbidden_task_stations": [int(task["station"] in task["forbidden_stations"]) for task in tasks],
        "same_station_groups": [
            len({station[task_id] for task_id in group["task_ids"]}) - 1
            for group in problem.get("same_station_groups") or []
        ],
        "different_station_pairs": [
            int(station[pair["first"]] == station[pair["second"]])
            for pair in problem.get("different_station_pairs") or []
        ],
        "minimize_stations": [sum(load > 0 for load in station_loads)],
        "workload_balance": [
            max((load for load in station_loads if load), default=0)
            - min((load for load in station_loads if load), default=0)
        ],
    }


def totals(problem: Mapping[str, Any]) -> dict[str, int]:
    """Per-constraint raw totals under the 0.2.0 names (the comparable quantity)."""
    return {RENAMED[name]: sum(values) for name, values in constraint_costs(problem).items()}


def score_costs(
    costs: Mapping[str, Sequence[float]],
    picks: Sequence[tuple[str, float | None, bool | None, float]] | None = None,
) -> tuple[int, float]:
    """0.1.5 ``common.score_costs``: picks are ``(old name, weight, hard, exponent)``; None = defaults.

    Each element is shaped sign-preserving by the exponent, the weighted totals
    are summed per level, and the hard total is rounded ONCE (half away from zero).
    """
    selected = [(name, None, None, 1.0) for name in OLD_NAMES] if picks is None else list(picks)
    if not selected:
        raise ValueError("constraints must not be empty (pass None for defaults)")
    hard = 0.0
    soft = 0.0
    for name, weight, is_hard, exponent in selected:
        if name not in costs:
            raise ValueError(f"unknown constraint: {name}")
        default_hard, default_weight = OLD_DEFAULTS[name]
        weight = default_weight if weight is None else weight
        is_hard = default_hard if is_hard is None else is_hard
        if exponent <= 0:
            raise ValueError("constraint exponent must be positive")
        total = sum(math.copysign(abs(value) ** exponent, value) for value in costs[name])
        if is_hard:
            hard += weight * total
        else:
            soft += weight * total
    rounded_hard = int(math.floor(hard + 0.5) if hard >= 0 else math.ceil(hard - 0.5))
    return -rounded_hard, (-soft if soft else 0.0)


def default_score(problem: Mapping[str, Any]) -> tuple[int, float]:
    """0.1.5 ``calculate_score`` at the default constraint set."""
    return score_costs(constraint_costs(problem))


def topological_order(problem: Mapping[str, Any]) -> list[str]:
    """0.1.5 construction, step 1: level-synchronous Kahn order, ids sorted within a level."""
    tasks = _tasks(problem)
    predecessor_ids: dict[str, list[str]] = {task["id"]: [] for task in tasks}
    for edge in problem.get("precedences") or []:
        predecessor_ids[edge["after"]].append(edge["before"])
    remaining = {task["id"] for task in tasks}
    ordered: list[str] = []
    while remaining:
        ready = sorted(
            task_id for task_id in remaining if all(pred not in remaining for pred in predecessor_ids[task_id])
        )
        if not ready:
            raise ValueError("precedence graph must be acyclic")
        ordered.extend(ready)
        remaining.difference_update(ready)
    return ordered


def construct(problem: Mapping[str, Any]) -> list[int]:
    """0.1.5 construction, step 2: the initial stations ``solve`` started its search from.

    A warm start (``warm_start=True``) keeps its seeded stations (>= 0); the
    others go first fit, in topological order, onto a station pointer that
    never moves down (a seed pushes it up to its station); finally every
    unseeded task with a ``fixed_station`` is overwritten to it, without
    updating the loads. Returns the station per task, in task order.
    """
    tasks = _tasks(problem)
    cycle_time = problem["cycle_time"]
    ordered = topological_order(problem)
    by_id = {task["id"]: index for index, task in enumerate(tasks)}
    initial = [task["station"] if problem.get("warm_start", False) else -1 for task in tasks]
    seeded = {index for index, assigned in enumerate(initial) if assigned >= 0}
    station_loads: dict[int, int] = defaultdict(int)
    for index in seeded:
        station_loads[initial[index]] += tasks[index]["duration"]
    station = 0
    for task_id in ordered:
        task = tasks[by_id[task_id]]
        if by_id[task_id] in seeded:
            station = max(station, initial[by_id[task_id]])
            continue
        while station_loads[station] and station_loads[station] + task["duration"] > cycle_time:
            station += 1
        initial[by_id[task_id]] = station
        station_loads[station] += task["duration"]
    for index, task in enumerate(tasks):
        if task["fixed_station"] is not None and index not in seeded:
            initial[index] = task["fixed_station"]
    return initial


def restricted_to_assigned(problem: Mapping[str, Any]) -> dict[str, Any]:
    """The problem with every unassigned task (and what touches it) left out.

    The 0.2.0 engine convention, stated in 0.1.5 terms: an unassigned task
    contributes nothing, so its arcs and pairs do not count and it is not a
    member of any group (a group with no assigned member is left out: it
    cannot be split). ``constraint_costs`` of the result equals the C++ port's
    raw totals on the original, partial assignment.
    """
    kept = [dict(task) for task in problem["tasks"] if task.get("station", -1) >= 0]
    ids = {task["id"] for task in kept}
    groups = []
    for group in problem.get("same_station_groups") or []:
        members = [task_id for task_id in group["task_ids"] if task_id in ids]
        if members:
            groups.append({"id": group["id"], "task_ids": members})
    return {
        **{key: value for key, value in problem.items() if key not in ("tasks", "precedences")},
        "tasks": kept,
        "precedences": [
            edge for edge in problem.get("precedences") or [] if edge["before"] in ids and edge["after"] in ids
        ],
        "same_station_groups": groups,
        "different_station_pairs": [
            pair
            for pair in problem.get("different_station_pairs") or []
            if pair["first"] in ids and pair["second"] in ids
        ],
    }

from __future__ import annotations

from ._core import SolverConfig as SolverConfig
from .common import anneal, configure
from .constraints import DEFAULTS, score
from .models import AssemblyLineBalancingProblemModel


def list_constraints() -> list[str]:
    return [constraint.name for constraint in DEFAULTS]


def _loads(problem):
    count = max((task.station for task in problem.tasks), default=-1) + 1
    loads = [0] * count
    for task in problem.tasks:
        if task.station >= 0:
            loads[task.station] += task.duration
    return loads


def constraint_costs(problem: AssemblyLineBalancingProblemModel) -> dict:
    loads = _loads(problem)
    station = {task.id: task.station for task in problem.tasks}
    reversed_arcs = sum(station[edge.before] > station[edge.after] for edge in problem.precedences)
    return {
        "station_capacity": [max(0, load - problem.cycle_time) for load in loads],
        "task_precedence": [reversed_arcs],
        "station_count_limit": [
            0 if problem.max_stations is None else max(0, task.station - problem.max_stations + 1)
            for task in problem.tasks
        ],
        "task_station_bounds": [
            max(0, task.min_station - task.station)
            + (0 if task.max_station is None else max(0, task.station - task.max_station))
            for task in problem.tasks
        ],
        "fixed_task_stations": [
            int(task.fixed_station is not None and task.station != task.fixed_station) for task in problem.tasks
        ],
        "forbidden_task_stations": [int(task.station in task.forbidden_stations) for task in problem.tasks],
        "same_station_groups": [
            len({station[task_id] for task_id in group.task_ids}) - 1 for group in problem.same_station_groups
        ],
        "different_station_pairs": [
            int(station[pair.first] == station[pair.second]) for pair in problem.different_station_pairs
        ],
        "minimize_stations": [sum(load > 0 for load in loads)],
        "workload_balance": [
            max((load for load in loads if load), default=0) - min((load for load in loads if load), default=0)
        ],
    }


def calculate_score(problem: AssemblyLineBalancingProblemModel, constraints=None):
    problem.station_loads = _loads(problem)
    problem.stations_used = sum(load > 0 for load in problem.station_loads)
    result = score(constraint_costs(problem), constraints)
    problem.score_hard, problem.score_soft = result
    return result


def solve(
    problem, *, time_limit_seconds=None, random_seed=None, parallel_solver_count=None, config=None, constraints=None
):
    cfg = configure(config, time_limit_seconds, random_seed, parallel_solver_count)
    predecessor_ids = {task.id: [] for task in problem.tasks}
    for edge in problem.precedences:
        predecessor_ids[edge.after].append(edge.before)
    remaining = {task.id for task in problem.tasks}
    ordered = []
    while remaining:
        ready = sorted(
            task_id for task_id in remaining if all(pred not in remaining for pred in predecessor_ids[task_id])
        )
        if not ready:
            raise ValueError("precedence graph must be acyclic")
        ordered.extend(ready)
        remaining.difference_update(ready)
    by_id = {task.id: index for index, task in enumerate(problem.tasks)}
    initial = [0] * len(problem.tasks)
    station = 0
    load = 0
    for task_id in ordered:
        task = problem.tasks[by_id[task_id]]
        if load and load + task.duration > problem.cycle_time:
            station += 1
            load = 0
        initial[by_id[task_id]] = station
        load += task.duration
    for index, task in enumerate(problem.tasks):
        if task.fixed_station is not None:
            initial[index] = task.fixed_station

    def evaluate(state):
        for task, selected_station in zip(problem.tasks, state, strict=True):
            task.station = selected_station
        return calculate_score(problem, constraints)

    def neighbor(state, rng):
        result = state.copy()
        if result:
            index = rng.randrange(len(result))
            result[index] = rng.randrange(max(result, default=0) + 2)
        return result

    if problem.tasks:
        best, _ = anneal(initial, neighbor, evaluate, cfg)
        evaluate(best)
    else:
        calculate_score(problem, constraints)
    return problem.score_hard, problem.score_soft

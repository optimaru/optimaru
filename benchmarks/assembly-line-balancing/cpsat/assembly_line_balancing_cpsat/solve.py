"""Exact CP-SAT formulation for assembly-line balancing.

The returned object uses the canonical ``AssemblyLineBalancingProblemModel``
JSON contract. Every task goes on exactly one station 0..n-1 (n = task count,
the engine's value range too); hard costs are optimized lexicographically
before the soft ones, matching the engine's ``HardSoftScore``, and the result
is re-scored by the shared C++ evaluator (``constraint_costs``) under the same
selection.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass

from assembly_line_balancing import calculate_score, constraint_costs
from assembly_line_balancing.constraints import Constraint
from assembly_line_balancing.models import AssemblyLineBalancingProblemModel
from cpsat_plugins import ConstraintConfig, ObjectiveBuilder
from ortools.sat.python import cp_model

from .constraints import REGISTRY, LineContext

__all__ = ["Built", "build", "catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


@dataclass(frozen=True)
class Built:
    """A built model: the CP-SAT model, its variables and the selected objective."""

    model: cp_model.CpModel
    context: LineContext
    objective: ObjectiveBuilder


def _symmetric(problem: AssemblyLineBalancingProblemModel) -> bool:
    """Whether relabelling the used stations 0, 1, 2, ... (in order) never costs more.

    Compacting keeps loads, the order of the stations (precedence), equal / unequal
    stations (zoning) and only moves tasks down (max_station, max_stations), so it is
    safe unless a task has a lower station bound, a fixed station or forbidden stations.
    """
    return not any(
        task.min_station > 0 or task.fixed_station is not None or task.forbidden_stations for task in problem.tasks
    )


def build(
    problem: AssemblyLineBalancingProblemModel,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
    *,
    symmetry_breaking: bool | None = None,
) -> Built:
    """Build the CP-SAT model of `problem` under a constraint selection (None = the defaults).

    ``symmetry_breaking`` (None = when it is safe, see `_symmetric`) makes the used
    stations a prefix 0..k-1; pass False to pin an arbitrary assignment.
    """
    model = cp_model.CpModel()
    count = len(problem.tasks)
    index = {task.id: position for position, task in enumerate(problem.tasks)}
    assigned = {
        (task, station): model.new_bool_var(f"task_{task}_station_{station}")
        for task in range(count)
        for station in range(count)
    }
    station_of = [model.new_int_var(0, max(0, count - 1), f"station_of_{task}") for task in range(count)]
    used = [model.new_bool_var(f"station_{station}_used") for station in range(count)]
    for task in range(count):
        model.add_exactly_one(assigned[task, station] for station in range(count))
        model.add(station_of[task] == sum(station * assigned[task, station] for station in range(count)))
    loads = []
    for station in range(count):
        model.add_max_equality(used[station], [assigned[task, station] for task in range(count)])
        loads.append(sum(problem.tasks[task].duration * assigned[task, station] for task in range(count)))
    if symmetry_breaking if symmetry_breaking is not None else _symmetric(problem):
        for station in range(count - 1):
            model.add(used[station] >= used[station + 1])

    # Default weights and levels come from the engine (constraint_info()); the
    # canonical problem carries none.
    objective = REGISTRY.objective(model, ("hard", "soft"), constraints)
    context = LineContext(model, problem, index, assigned, station_of, used, loads)
    REGISTRY.build(context, objective)
    return Built(model, context, objective)


def solve(
    problem: AssemblyLineBalancingProblemModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
) -> AssemblyLineBalancingProblemModel:
    """Solve ``problem`` without mutating it and return a scored solution."""
    built = build(problem, constraints)
    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = built.objective.solve(solver)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(f"CP-SAT did not find a solution (status={solver.status_name(status)})")

    solved = problem.model_copy(deep=True)
    solved.warm_start = False
    for position, task in enumerate(solved.tasks):
        task.station = solver.value(built.context.station_of[position])

    core = solved.to_core()
    calculate_score(core)
    result = AssemblyLineBalancingProblemModel.from_core(core)
    selected_score = built.objective.score_costs(constraint_costs(core))
    result.score_hard = selected_score["hard"]
    result.score_soft = float(selected_score["soft"])
    return result

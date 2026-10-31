"""Exact CP-SAT formulation for the max-cut use case.

One bool per vertex (its side) and one per edge (crossed); every constraint is a
plugin (``constraints.py``) that adds the engine component's raw cost. The
returned object uses the canonical ``MaxCutProblemModel`` JSON contract and is
rescored by the engine's own ``constraint_costs`` under the same selection. Hard
terms are optimized lexicographically before soft ones, matching the engine's
``HardSoftScore``.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass

from cpsat_plugins import ConstraintConfig, ObjectiveBuilder
from max_cut import calculate_score, constraint_costs
from max_cut.constraints import Constraint
from max_cut.models import MaxCutProblemModel
from ortools.sat.python import cp_model

from .constraints import REGISTRY, MaxCutContext, _differs

__all__ = ["Built", "build", "catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


@dataclass(frozen=True)
class Built:
    model: cp_model.CpModel
    context: MaxCutContext
    objective: ObjectiveBuilder


def build(
    problem: MaxCutProblemModel,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
    *,
    symmetry_breaking: bool = True,
) -> Built:
    """The CP-SAT model of ``problem`` under a constraint selection (None = the defaults).

    ``symmetry_breaking`` puts the first vertex on side 0 when nothing is fixed:
    every constraint is then symmetric under swapping the sides, so no optimum is
    lost. Turn it off to pin arbitrary partitions (the parity tests).
    """
    model = cp_model.CpModel()
    side = {vertex: model.new_bool_var(f"side_{index}") for index, vertex in enumerate(problem.vertices)}
    crossed = [
        _differs(model, side[edge.source], side[edge.target], f"edge_{index}_crossed")
        for index, edge in enumerate(problem.edges)
    ]
    if symmetry_breaking and problem.vertices and not problem.fixed_partition:
        model.add(side[problem.vertices[0]] == 0)
    context = MaxCutContext(model, problem, side, crossed)
    # Default weights and levels come from the engine (constraint_info()); the
    # canonical problem carries none.
    objective = REGISTRY.objective(model, ("hard", "soft"), constraints)
    REGISTRY.build(context, objective)
    return Built(model, context, objective)


def solve(
    problem: MaxCutProblemModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
    symmetry_breaking: bool = True,
) -> MaxCutProblemModel:
    """Solve ``problem`` without mutating it and return a scored solution."""
    built = build(problem, constraints, symmetry_breaking=symmetry_breaking)
    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = built.objective.solve(solver)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(f"CP-SAT did not find a solution (status={solver.status_name(status)})")

    solved = problem.model_copy(deep=True)
    solved.partition = {vertex: int(solver.value(variable)) for vertex, variable in built.context.side.items()}
    core = solved.to_core()
    calculate_score(core)
    result = MaxCutProblemModel.from_core(core)
    result.warm_start = problem.warm_start
    selected_score = built.objective.score_costs(constraint_costs(core))
    result.score_hard = selected_score["hard"]
    result.score_soft = float(selected_score["soft"])
    return result

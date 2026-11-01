"""Exact CP-SAT formulation for the knapsack use case.

One bool per item; every constraint is a plugin (``constraints.py``) that adds
the engine component's raw cost. The returned object uses the canonical
``KnapsackProblemModel`` JSON contract and is rescored by the engine's own
``constraint_costs`` under the same selection. Hard terms are optimized
lexicographically before soft ones, matching the engine's ``HardSoftScore``.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass

from cpsat_plugins import ConstraintConfig, ObjectiveBuilder
from knapsack import calculate_score, constraint_costs
from knapsack.constraints import Constraint
from knapsack.models import KnapsackProblemModel
from ortools.sat.python import cp_model

from .constraints import REGISTRY, KnapsackContext

__all__ = ["Built", "build", "catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


@dataclass(frozen=True)
class Built:
    model: cp_model.CpModel
    context: KnapsackContext
    objective: ObjectiveBuilder


def build(
    problem: KnapsackProblemModel,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
) -> Built:
    """The CP-SAT model of ``problem`` under a constraint selection (None = the defaults)."""
    model = cp_model.CpModel()
    chosen = [model.new_bool_var(f"selected_{index}") for index in range(len(problem.items))]
    by_id = {item.id: chosen[index] for index, item in enumerate(problem.items)}
    context = KnapsackContext(model, problem, chosen, by_id)
    # Default weights and levels come from the engine (constraint_info()); the
    # canonical problem carries none.
    objective = REGISTRY.objective(model, ("hard", "soft"), constraints)
    REGISTRY.build(context, objective)
    return Built(model, context, objective)


def solve(
    problem: KnapsackProblemModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
) -> KnapsackProblemModel:
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
    for index, item in enumerate(solved.items):
        item.selected = bool(solver.value(built.context.chosen[index]))
    core = solved.to_core()
    calculate_score(core)
    result = KnapsackProblemModel.from_core(core)
    result.warm_start = problem.warm_start
    selected_score = built.objective.score_costs(constraint_costs(core))
    result.score_hard = selected_score["hard"]
    result.score_soft = float(selected_score["soft"])
    return result

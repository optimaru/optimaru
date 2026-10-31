"""Exact CP-SAT formulation for the quadratic-assignment use case.

One bool per (facility, location) pair with exactly one location per facility
and one facility per location; every constraint is a plugin (``constraints.py``)
that adds the engine component's raw cost. The returned object uses the
canonical ``QuadraticAssignmentProblemModel`` JSON contract and is rescored by
the engine's own ``constraint_costs`` under the same selection. Hard terms are
optimized lexicographically before soft ones, matching the engine's
``HardSoftScore``.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass

from cpsat_plugins import ConstraintConfig, ObjectiveBuilder
from ortools.sat.python import cp_model
from quadratic_assignment import calculate_score, constraint_costs
from quadratic_assignment.constraints import Constraint
from quadratic_assignment.models import QuadraticAssignmentProblemModel

from .constraints import REGISTRY, QapContext

__all__ = ["Built", "build", "catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


@dataclass(frozen=True)
class Built:
    model: cp_model.CpModel
    context: QapContext
    objective: ObjectiveBuilder


def build(
    problem: QuadraticAssignmentProblemModel,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
) -> Built:
    """The CP-SAT model of ``problem`` under a constraint selection (None = the defaults)."""
    model = cp_model.CpModel()
    size = len(problem.facilities)
    assigned = {
        (facility, location): model.new_bool_var(f"f{facility}_l{location}")
        for facility in range(size)
        for location in range(size)
    }
    for facility in range(size):
        model.add_exactly_one(assigned[facility, location] for location in range(size))
    for location in range(size):
        model.add_exactly_one(assigned[facility, location] for facility in range(size))
    context = QapContext(
        model,
        problem,
        assigned,
        {name: index for index, name in enumerate(problem.facilities)},
        {name: index for index, name in enumerate(problem.locations)},
    )
    # Default weights and levels come from the engine (constraint_info()); the
    # canonical problem carries none.
    objective = REGISTRY.objective(model, ("hard", "soft"), constraints)
    REGISTRY.build(context, objective)
    return Built(model, context, objective)


def solve(
    problem: QuadraticAssignmentProblemModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
) -> QuadraticAssignmentProblemModel:
    """Solve ``problem`` without mutating it and return a scored solution."""
    built = build(problem, constraints)
    # Any permutation is feasible: hint the identity (the engine's start) so a
    # short budget still returns a solution while the O(n^4) model presolves.
    for (facility, location), literal in built.context.assigned.items():
        built.model.add_hint(literal, facility == location)
    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = built.objective.solve(solver)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(f"CP-SAT did not find a solution (status={solver.status_name(status)})")

    size = len(problem.facilities)
    solved = problem.model_copy(deep=True)
    solved.assignment = {
        problem.locations[location]: problem.facilities[
            next(facility for facility in range(size) if solver.value(built.context.assigned[facility, location]))
        ]
        for location in range(size)
    }
    core = solved.to_core()
    calculate_score(core)
    result = QuadraticAssignmentProblemModel.from_core(core)
    result.warm_start = problem.warm_start
    selected_score = built.objective.score_costs(constraint_costs(core))
    result.score_hard = selected_score["hard"]
    result.score_soft = float(selected_score["soft"])
    return result

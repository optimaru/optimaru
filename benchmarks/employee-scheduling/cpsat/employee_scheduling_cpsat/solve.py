"""CP-SAT shift assignment model for employee scheduling."""

from __future__ import annotations

from typing import Any

from es_scheduling import DecomposedCalculator
from es_scheduling.models import ScheduleRequest, to_schedule
from ortools.sat.python import cp_model

from .constraints import REGISTRY, ScheduleContext, registry_for

__all__ = ["catalog", "constraint_costs", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def constraint_costs(request: ScheduleRequest) -> dict[str, Any]:
    """Score the workers set on `request` with the engine's own calculator.

    The calculator is the one the engine searches with, built from the same
    rule list, so the two solvers are compared on one score.
    """
    model_input = request.model_input
    calculator = DecomposedCalculator(model_input.rules)
    result = calculator.score(to_schedule(model_input))
    return {
        "hard": result.hard,
        "soft": result.soft,
        "totals": calculator.constraint_totals(),
    }


def solve(
    problem: ScheduleRequest,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    _fixed_workers: list[int | None] | None = None,
    _proxy_score: dict[str, int] | None = None,
) -> ScheduleRequest:
    """Assign workers to shifts, minimizing hard cost before soft cost.

    The rules come from `problem.model_input.rules`. A shift may stay
    unassigned only when the unassigned_shift rule prices it; without that rule
    the engine assigns every shift, so this model does too. The returned copy
    holds the answer in `ShiftModel.worker`.
    """
    model_input = problem.model_input
    rules = model_input.rules
    shifts, workers = model_input.shifts, model_input.workers
    if any(shift.pinned for shift in shifts):
        raise ValueError(
            "pinned shifts are not supported: the run and week rules exempt "
            "pinned-only stretches, which this model does not express"
        )
    # Refuses what the engine refuses, such as a scope on a global rule.
    DecomposedCalculator(rules)

    model = cp_model.CpModel()
    x = {
        (s, w): model.new_bool_var(f"shift_{s}_worker_{w}")
        for s in range(len(shifts))
        for w in range(len(workers))
    }
    unassigned = [model.new_bool_var(f"shift_{s}_unassigned") for s in range(len(shifts))]
    may_stay_empty = any(rule.name == "unassigned_shift" for rule in rules)
    for s in range(len(shifts)):
        model.add_exactly_one([*(x[s, w] for w in range(len(workers))), unassigned[s]])
        if _fixed_workers is not None:
            if _fixed_workers[s] is None:
                model.add(unassigned[s] == 1)
            else:
                model.add(x[s, _fixed_workers[s]] == 1)
        elif not may_stay_empty:
            model.add(unassigned[s] == 0)

    registry = registry_for(rules)
    objective = registry.objective(model, ("hard", "soft"), None)
    registry.build(ScheduleContext(model, problem, x, unassigned), objective)

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = objective.solve(solver)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(
            f"CP-SAT did not find a solution (status={solver.status_name(status)})"
        )
    if _proxy_score is not None:
        _proxy_score.update(objective.score(solver))

    solved = problem.model_copy(deep=True)
    for s, shift in enumerate(solved.model_input.shifts):
        shift.worker = next(
            (workers[w].id for w in range(len(workers)) if solver.value(x[s, w])), None
        )
    return solved

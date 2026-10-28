"""Exact CP-SAT formulation for facility location.

The returned object uses the canonical ``FacilityLocationProblemModel`` JSON
contract.  Hard capacity overflow is optimized lexicographically before the
weighted setup and distance cost, matching the engine's ``HardSoftScore``.
"""

from __future__ import annotations

import math
from collections.abc import Iterable, Mapping

from ortools.sat.python import cp_model

from facility_location import calculate_score, constraint_costs
from facility_location.constraints import Constraint
from facility_location.models import FacilityLocationProblemModel
from cpsat_plugins import ConstraintConfig

from .constraints import FacilityContext, REGISTRY

__all__ = ["catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def _distance_meters(a, b) -> int:
    # Mirrors fl::Location::distanceTo.
    return math.ceil(
        math.hypot(a.latitude - b.latitude, a.longitude - b.longitude) * 111_000.0
    )


def solve(
    problem: FacilityLocationProblemModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
) -> FacilityLocationProblemModel:
    """Solve ``problem`` without mutating it and return a scored solution."""
    if not problem.facilities and problem.consumers:
        raise ValueError("at least one facility is required when consumers exist")

    model = cp_model.CpModel()
    consumer_count = len(problem.consumers)
    facility_count = len(problem.facilities)
    x = {
        (c, f): model.new_bool_var(f"consumer_{c}_facility_{f}")
        for c in range(consumer_count)
        for f in range(facility_count)
    }
    used = [model.new_bool_var(f"facility_{f}_used") for f in range(facility_count)]

    for c in range(consumer_count):
        model.add_exactly_one(x[c, f] for f in range(facility_count))
    for f in range(facility_count):
        assigned = [x[c, f] for c in range(consumer_count)]
        if assigned:
            model.add_max_equality(used[f], assigned)
        else:
            model.add(used[f] == 0)

    overflow = []
    for f, facility in enumerate(problem.facilities):
        total_demand = sum(c.demand for c in problem.consumers)
        excess = model.new_int_var(0, total_demand, f"facility_{f}_overflow")
        load = sum(problem.consumers[c].demand * x[c, f] for c in range(consumer_count))
        model.add(excess >= load - facility.capacity)
        overflow.append(excess)

    distances = [
        [
            _distance_meters(consumer.location, facility.location)
            for facility in problem.facilities
        ]
        for consumer in problem.consumers
    ]
    objective = REGISTRY.objective(
        model,
        ("hard", "soft"),
        constraints,
        default_weights={
            "facility_setup_cost": problem.setup_cost_weight,
            "distance_from_facility": problem.distance_from_facility_weight,
        },
    )
    REGISTRY.build(
        FacilityContext(model, problem, x, used, overflow, distances), objective
    )
    objective.minimize()

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = solver.solve(model)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(
            f"CP-SAT did not find a solution (status={solver.status_name(status)})"
        )

    solved = problem.model_copy(deep=True)
    for c, consumer in enumerate(solved.consumers):
        consumer.facility = next(
            f for f in range(facility_count) if solver.value(x[c, f])
        )

    core = solved.to_core()
    calculate_score(core)
    result = FacilityLocationProblemModel.from_core(core)
    selected_score = objective.score_costs(constraint_costs(core))
    result.score_hard = selected_score["hard"]
    result.score_soft = float(selected_score["soft"])
    return result

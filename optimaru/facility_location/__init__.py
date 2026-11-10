"""Facility location — Python wrapper of the C++ solver on the optimaru engine.

The facility-location use case: one assignment-shape decision variable
(`Consumer.facility`, a facility index, -1 = unassigned) picks one facility
per consumer.

Score, at the default weights:
  hard = -(sum over used facilities of max(0, assigned demand - capacity))  # facility_capacity_cost
  soft = -(2 x setup cost of used facilities                                 # facility_setup_cost
           + 6 x consumer-facility distance)                                 # distance_from_facility_cost
The problem carries no weights: change one through a constraint
selection, e.g. ``solve(problem, constraints=get_constraints([..., DistanceFromFacilityCost(weight=5)]))``
(see `optimaru.facility_location.constraints`). `constraint_info()` lists every constraint with its
default level and weight.

Basic usage::

    import optimaru.facility_location as fl

    problem = fl.FacilityLocationProblem(facilities, consumers)
    fl.solve(problem, time_limit_seconds=5.0, random_seed=42)
    print(problem.score, problem.total_cost(), problem.total_distance_meters())

The pydantic mirror (for serialization) is in `optimaru.facility_location.models` (optional dependency).
"""

import copy

from ._core import (
    ConstraintInfo,
    Consumer,
    Facility,
    FacilityLocationProblem,
    Location,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    constraint_info,
    list_constraints,
)

__all__ = [
    "ConstraintInfo",
    "Consumer",
    "Facility",
    "FacilityLocationProblem",
    "Location",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "solve",
    "list_constraints",
    "constraint_costs",
    "constraint_info",
    "with_initial_solution",
]


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    See `models.with_initial_solution`. Lazy import: pydantic is an optional dependency.
    """
    from . import models

    return models.with_initial_solution(problem, solution)


def solve(
    problem: FacilityLocationProblem,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
):
    """High-level API that solves `problem` in place and returns it.

    The score is on the result: ``solve(problem).score`` (``.hard``, ``.soft``).

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, ALNS
    on). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached.

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function directly
    (see `optimaru.facility_location.constraints`). Only the listed constraints are scored;
    None uses all three default constraints at their default weights::

        from optimaru.facility_location.constraints import get_constraints
        fl.solve(problem, constraints=get_constraints())
    """
    # A copy: the keyword overrides below must not leak into the caller's config.
    cfg = copy.copy(config) if config is not None else SolverConfig()
    if time_limit_seconds is not None:
        cfg.spent_limit_seconds = time_limit_seconds
    if random_seed is not None:
        cfg.random_seed = random_seed
    if parallel_solver_count is not None:
        cfg.parallel_solver_count = parallel_solver_count
    if stop_at_first_feasible:
        cfg.best_score_limit = (0, float("-inf"))
    if full_assert:
        cfg.full_assert = True
    if on_new_best is not None:
        cfg.on_new_best = on_new_best

    if constraints is None:
        Solver(cfg).solve(problem)
    else:
        Solver(cfg).solve_with_constraints(problem, constraints)
    return problem

"""Facility location — Python wrapper of the C++ solver on the optimaru engine.

A port of the facility-location example model: one BASIC planning variable
(`Consumer.facility`, a facility index, -1 = unassigned) picks one facility
per consumer.

Score (HardSoftScore):
  hard = -(sum over used facilities of max(0, assigned demand - capacity))  # capacity
  soft = -(2 x setup cost of used facilities + 6 x consumer-facility distance)  # setup + distance
The distance weight 6 is the ConstraintWeightOverrides value of the Java solution (the
provider default is 5); change it with `problem.distance_from_facility_weight`.

Basic usage::

    import facility_location as fl

    problem = fl.FacilityLocationProblem(facilities, consumers)
    fl.solve(problem, time_limit_seconds=5.0, random_seed=42)
    print(problem.score, problem.total_cost(), problem.total_distance_meters())

The pydantic mirror (for serialization) is in `facility_location.models` (optional dependency).
"""

from ._core import (
    Consumer,
    Facility,
    FacilityLocationProblem,
    Location,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    list_constraints,
)

__all__ = [
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
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> Score:
    """High-level API that solves `problem` in place and returns the final `Score`.

    ``config`` becomes the base settings (None = engine defaults: 30 s budget, ALNS
    on). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached.

    ``constraints`` picks the cost function directly
    (see `facility_location.constraints`). Only the listed constraints are scored;
    None uses all three default constraints::

        from facility_location.constraints import get_constraints
        fl.solve(problem, constraints=get_constraints())
    """
    cfg = config if config is not None else SolverConfig()
    if time_limit_seconds is not None:
        cfg.spent_limit_seconds = time_limit_seconds
    if random_seed is not None:
        cfg.random_seed = random_seed
    if parallel_solver_count is not None:
        cfg.parallel_solver_count = parallel_solver_count
    if stop_at_first_feasible:
        cfg.best_score_limit = (0, -1e18)
    if on_new_best is not None:
        cfg.on_new_best = on_new_best

    if constraints is None:
        Solver(cfg).solve(problem)
    else:
        Solver(cfg).solve_with_constraints(problem, constraints)
    return problem.score

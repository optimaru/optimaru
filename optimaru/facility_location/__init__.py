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

The pydantic mirror (for serialization) is in `optimaru.facility_location.models`.
"""

from typing import TypeVar

from .._solve import solve_in_place, solver_config
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
from .models import FacilityLocationProblemModel, solve_request, with_initial_solution

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
    "constraint_costs",
    "constraint_info",
    "list_constraints",
    "solve",
    "solve_request",
    "with_initial_solution",
]


# solve() returns what it was given: the compiled problem, or its pydantic model.
_Problem = TypeVar("_Problem")


def solve(
    problem: _Problem,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> _Problem:
    """High-level API that solves `problem` in place and returns it.

    `problem` is a `FacilityLocationProblem` (solved in place and returned) or a
    `models.FacilityLocationProblemModel` (converted, solved, and returned as a new model; the
    one passed in is left as it was).

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
    cfg = solver_config(
        SolverConfig,
        config,
        time_limit_seconds=time_limit_seconds,
        random_seed=random_seed,
        parallel_solver_count=parallel_solver_count,
        stop_at_first_feasible=stop_at_first_feasible,
        full_assert=full_assert,
        on_new_best=on_new_best,
    )
    if isinstance(problem, FacilityLocationProblem):
        solve_in_place(Solver(cfg), problem, constraints)
        return problem
    if isinstance(problem, FacilityLocationProblemModel):
        core = problem.to_core()
        solve_in_place(Solver(cfg), core, constraints)
        return FacilityLocationProblemModel.from_core(core)
    raise TypeError(
        f"problem must be a FacilityLocationProblem or a models.FacilityLocationProblemModel: {type(problem)!r}"
    )

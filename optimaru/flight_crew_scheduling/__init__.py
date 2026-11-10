"""Flight crew scheduling — Python wrapper of the C++ solver on the optimaru engine.

The flight-crew-scheduling use case: an assignment-shaped problem
that picks a crew member (employee) for every crew seat of a flight (FlightAssignment).

Constraints, at the default weights:
  hard 1. required_skill_cost                  -100 per assignment without the skill
  hard 2. flight_conflict_cost                 -10 per time-overlapping assignment pair of one
                                               crew member
  hard 3. transfer_between_two_flights_cost    -1 per pair of departure-consecutive assignments
                                               whose airports do not connect (arrival airport !=
                                               next departure airport)
  hard 4. employee_unavailability_cost         -10 per assignment of a crew member unavailable on
                                               the flight date
  soft 5. first_assignment_not_departing_from_home_cost  -1000 when the first assignment departs
                                               from an airport other than home
  soft 6. last_assignment_not_arriving_at_home_cost      -1000 when the last assignment arrives at
                                               an airport other than home
The weights are the C++ components' defaults; change one through a constraint selection, e.g.
``solve(schedule, constraints=[..., RequiredSkillCost(weight=500)])`` (see
`optimaru.flight_crew_scheduling.constraints`). `constraint_info()` lists every constraint with its default level
and weight.

The high-level entry point is `solve`. It takes a pydantic model (optional dependency)
or a dict and returns the solved model::

    import optimaru.flight_crew_scheduling as fc

    result = fc.solve(problem_dict, time_limit_seconds=10.0, random_seed=42)
    print(result.score_hard, result.score_soft)

There is also a low-level path that builds engine objects (`FlightCrewSchedule`) directly
and uses `Solver` (no pydantic needed). The pydantic mirror is in `optimaru.flight_crew_scheduling.models`.
"""

from typing import TypeVar

from .._solve import solve_in_place, solver_config
from ._core import (
    Airport,
    ConstraintInfo,
    Employee,
    Flight,
    FlightAssignment,
    FlightCrewSchedule,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    constraint_info,
    list_constraints,
)

__all__ = [
    "Airport",
    "ConstraintInfo",
    "Employee",
    "Flight",
    "FlightAssignment",
    "FlightCrewSchedule",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "list_constraints",
    "solve",
    "with_initial_solution",
]


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    See `models.with_initial_solution`. Lazy import: pydantic is an optional dependency.
    """
    from . import models

    return models.with_initial_solution(problem, solution)


# solve() returns what it was given: the compiled problem, or its pydantic model.
_Problem = TypeVar("_Problem")


def solve(
    schedule: _Problem,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config=None,
    constraints=None,
) -> _Problem:
    """High-level API that solves a schedule and returns it.

    ``schedule`` is one of three:
      - `models.FlightCrewScheduleModel` (pydantic) -> returns the solved model
      - a dict of the same shape -> validated, returns the solved model
      - `_core.FlightCrewSchedule` (engine object) -> solved in place and returned

    ``config`` becomes the base settings: pass a `_core.SolverConfig` as-is or an
    object with `to_core()` (`models.SolveConfig`). None = engine defaults
    (30 s budget). Explicit keyword arguments override config.

    ``stop_at_first_feasible`` stops as soon as hard 0 is reached
    (best_score_limit=(0, -inf)).

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function directly
    (see `optimaru.flight_crew_scheduling.constraints`). Only the listed constraints are scored, a name
    picked several times once per pick; None uses all six default constraints at
    their default weights::

        from optimaru.flight_crew_scheduling.constraints import get_constraints
        fcs.solve(schedule, constraints=get_constraints())
    """
    if config is not None and not isinstance(config, SolverConfig):
        if not hasattr(config, "to_core"):
            raise ValueError("config must be a SolverConfig or an object with to_core() (models.SolveConfig)")
        config = config.to_core()
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
    if isinstance(schedule, FlightCrewSchedule):
        solve_in_place(Solver(cfg), schedule, constraints)
        return schedule

    from .models import FlightCrewScheduleModel  # pydantic (optional dependency)

    model = (
        schedule if isinstance(schedule, FlightCrewScheduleModel) else FlightCrewScheduleModel.model_validate(schedule)
    )
    core = model.to_core()
    solve_in_place(Solver(cfg), core, constraints)
    return FlightCrewScheduleModel.from_core(core)

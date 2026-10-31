"""Flight crew scheduling — Python wrapper of the C++ solver on the optimaru engine.

A port of the `flight-crew-scheduling` example model: a BASIC-variable problem
that picks a crew member (employee) for every crew seat of a flight (FlightAssignment).

Constraints (HardSoftScore, with the Java ConstraintProvider's weights as-is):
  hard 1. requiredSkill               -100 per assignment without the skill
  hard 2. flightConflict              -10 per time-overlapping assignment pair of one crew member
  hard 3. transferBetweenTwoFlights   -1 per pair of departure-consecutive assignments whose
                                      airports do not connect (arrival airport != next
                                      departure airport)
  hard 4. employeeUnavailability      -10 per assignment of a crew member unavailable on the flight date
  soft 5. firstAssignmentNotDepartingFromHome  -1000 when the first assignment departs
                                      from an airport other than home
  soft 6. lastAssignmentNotArrivingAtHome      -1000 when the last assignment arrives at an
                                      airport other than home

The high-level entry point is `solve`. It takes a pydantic model (optional dependency)
or a dict and returns the solved model::

    import flight_crew as fc

    result = fc.solve(problem_dict, time_limit_seconds=10.0, random_seed=42)
    print(result.score_hard, result.score_soft)

There is also a low-level path that builds engine objects (`FlightCrewSchedule`) directly
and uses `Solver` (no pydantic needed). The pydantic mirror is in `flight_crew.models`.
"""

from ._core import (
    Airport,
    Employee,
    Flight,
    FlightAssignment,
    FlightCrewSchedule,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    list_constraints,
)

__all__ = [
    "Airport",
    "Employee",
    "Flight",
    "FlightAssignment",
    "FlightCrewSchedule",
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


def _resolve_config(config) -> SolverConfig:
    if config is None:
        return SolverConfig()
    if isinstance(config, SolverConfig):
        return config
    if hasattr(config, "to_core"):
        return config.to_core()
    raise ValueError("config must be a SolverConfig or an object with to_core() (models.SolveConfig)")


def solve(
    schedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config=None,
    constraints=None,
):
    """High-level API that solves a schedule and returns it.

    ``schedule`` is one of three:
      - `models.FlightCrewScheduleModel` (pydantic) -> returns the solved model
      - a dict of the same shape -> validated, returns the solved model
      - `_core.FlightCrewSchedule` (engine object) -> solved in place and returned

    ``config`` becomes the base settings: pass a `_core.SolverConfig` as-is or an
    object with `to_core()` (`models.SolveConfig`). None = engine defaults
    (30 s budget). Explicit keyword arguments override config.

    ``stop_at_first_feasible`` stops as soon as hard 0 is reached
    (best_score_limit=(0, -1e18)).

    ``constraints`` picks the cost function directly
    (see `flight_crew.constraints`). Only the listed constraints are scored; None
    uses all six default constraints::

        from flight_crew.constraints import get_constraints
        fcs.solve(schedule, constraints=get_constraints())
    """
    cfg = _resolve_config(config)
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

    def _solve(core):
        if constraints is None:
            Solver(cfg).solve(core)
        else:
            Solver(cfg).solve_with_constraints(core, constraints)

    if isinstance(schedule, FlightCrewSchedule):
        _solve(schedule)
        return schedule

    from .models import FlightCrewScheduleModel  # pydantic (optional dependency)

    model = (
        schedule if isinstance(schedule, FlightCrewScheduleModel) else FlightCrewScheduleModel.model_validate(schedule)
    )
    core = model.to_core()
    _solve(core)
    return FlightCrewScheduleModel.from_core(core)

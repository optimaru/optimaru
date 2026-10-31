"""Conference scheduling — Python wrapper of the C++ solver on the optimaru engine.

`Timeslot`/`Room`/`Speaker`/`Talk`/`ConferenceSchedule`/`Score`/`Breakdown`/
`Solver`/`SolverConfig`/`calculate_score`/`compute_breakdown` come from the compiled
C++ core (`_core`). `Talk.slot_room` is the planning variable
(-1 == unassigned; the value is the composite encoding timeslot_index * room_count + room_index),
and solve changes it and `score` in place::

    import conference_scheduling as cs

    schedule = cs.demo_conference()
    cs.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(schedule.score, schedule.assignment())

The pydantic mirror (for serialization) is in `conference_scheduling.models`
(optional dependency).
"""

from ._core import (
    Breakdown,
    ConferenceSchedule,
    Room,
    Score,
    Solver,
    SolverConfig,
    Speaker,
    Talk,
    Timeslot,
    calculate_score,
    compute_breakdown,
    constraint_costs,
    list_constraints,
    prepare,
)
from .demo import demo_conference

__all__ = [
    "Breakdown",
    "ConferenceSchedule",
    "Room",
    "Score",
    "Solver",
    "SolverConfig",
    "Speaker",
    "Talk",
    "Timeslot",
    "calculate_score",
    "compute_breakdown",
    "list_constraints",
    "constraint_costs",
    "demo_conference",
    "prepare",
    "solve",
    "with_initial_solution",
]


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    See `models.with_initial_solution`. Lazy import: pydantic is an optional dependency.
    """
    from . import models

    return models.with_initial_solution(problem, solution)


def solve(
    schedule: ConferenceSchedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> ConferenceSchedule:
    """High-level API that solves a `ConferenceSchedule` in place and returns it.

    ``config`` becomes the base settings (`_core.SolverConfig`, or an object with
    `to_core()` — `models.SolveConfig`). None = engine defaults (30 s budget).
    Explicit keyword arguments override config.

    ``stop_at_first_feasible`` stops as soon as hard 0 is reached
    (best_score_limit=(0, -1e18)).
    """
    if config is None:
        cfg = SolverConfig()
    elif isinstance(config, SolverConfig):
        cfg = config
    elif hasattr(config, "to_core"):
        cfg = config.to_core()
    else:
        raise ValueError("config must be a SolverConfig or an object with to_core()")
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
        Solver(cfg).solve(schedule)
    else:
        Solver(cfg).solve_with_constraints(schedule, constraints)
    return schedule

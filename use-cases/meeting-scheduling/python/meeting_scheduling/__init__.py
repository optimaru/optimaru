"""Meeting scheduling — Python wrapper of the C++ solver on the optimaru engine.

A port of the Java quickstart "meeting-scheduling": assigns meetings a starting time
grain (15-minute units) and a room. The Java entity's two planning variables
(startingTimeGrain, room) become one BASIC variable via the composite encoding
``grain_room = time_grain_pos * room_count + room_index`` (-1 == unassigned).
Constraints have the same semantics as the Java ConstraintProvider (5 hard + 2 medium +
5 soft; HardMediumSoftScore is folded as ``soft = -(medium * MEDIUM_SCALE + raw soft)``,
MEDIUM_SCALE == 1e6)::

    import meeting_scheduling as ms

    schedule = ms.demo_data()  # reproduces Java DemoDataGenerator (24 meetings)
    result = ms.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(result.score.hard, result.score.soft)

`solve` accepts both a `_core.MeetingSchedule` (solved in place and returned) and the
pydantic mirror `models.MeetingScheduleModel` (converted, solved, returned as a new model).
The pydantic mirror (for serialization) is in `meeting_scheduling.models` (optional dependency).
"""

from ._core import (
    GRAIN_LENGTH_IN_MINUTES,
    MEDIUM_SCALE,
    Breakdown,
    Meeting,
    MeetingAssignment,
    MeetingSchedule,
    Person,
    Room,
    Score,
    Solver,
    SolverConfig,
    TimeGrain,
    calculate_score,
    compute_breakdown,
    constraint_costs,
    demo_data,
    list_constraints,
)

__all__ = [
    "GRAIN_LENGTH_IN_MINUTES",
    "MEDIUM_SCALE",
    "Breakdown",
    "Meeting",
    "MeetingAssignment",
    "MeetingSchedule",
    "Person",
    "Room",
    "Score",
    "Solver",
    "SolverConfig",
    "TimeGrain",
    "calculate_score",
    "compute_breakdown",
    "list_constraints",
    "constraint_costs",
    "demo_data",
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
    schedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
):
    """High-level API that solves a schedule and returns it.

    ``schedule`` is a `_core.MeetingSchedule` (solved in place and returned)
    or a `models.MeetingScheduleModel` (converted, solved, returned as a new model).

    ``config`` becomes the base settings (None = engine defaults: 30 s budget);
    explicit keyword arguments override config. ``stop_at_first_feasible``
    stops as soon as hard 0 is reached (best_score_limit=(0, -1e18)).

    ``constraints`` picks the cost function directly
    (see `meeting_scheduling.constraints`). Only the listed constraints are scored;
    None uses all twelve default constraints::

        from meeting_scheduling.constraints import get_constraints
        ms.solve(schedule, constraints=get_constraints())
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

    def _solve(core):
        if constraints is None:
            Solver(cfg).solve(core)
        else:
            Solver(cfg).solve_with_constraints(core, constraints)

    if isinstance(schedule, MeetingSchedule):
        _solve(schedule)
        return schedule

    # pydantic mirror (lazy import: optional dependency).
    from .models import MeetingScheduleModel

    if isinstance(schedule, MeetingScheduleModel):
        core = schedule.to_core()
        _solve(core)
        return MeetingScheduleModel.from_core(core)
    raise TypeError(f"schedule must be a MeetingSchedule or a MeetingScheduleModel: {type(schedule)!r}")

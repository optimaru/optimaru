"""Meeting scheduling — Python wrapper of the C++ solver on the optimaru engine.

Assigns meetings a starting time grain (15-minute units) and a room. The two are
one decision variable per meeting, the composite encoding
``grain_room = time_grain_pos * room_count + room_index`` (-1 == unassigned).
There are 5 hard + 2 medium + 5 soft constraints; the medium level is folded into
the engine's soft level as ``soft = -(medium * MEDIUM_SCALE + raw soft)``,
MEDIUM_SCALE == 1e6. ``constraint_info()`` lists each constraint's default level and
weight (the medium pair at weight 1, in medium units); ``constraint_costs(schedule)`` gives
the raw per-constraint costs::

    import optimaru.meeting_scheduling as ms

    schedule = ms.generate_demo_data()  # the demo instance (24 meetings)
    result = ms.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(result.score.hard, result.score.soft)

`solve` accepts both a `_core.MeetingSchedule` (solved in place and returned) and the
pydantic mirror `models.MeetingScheduleModel` (converted, solved, and returned as a new model; the
one passed in is left as it was).
The pydantic mirror (for serialization) is in `optimaru.meeting_scheduling.models`.
"""

from typing import TypeVar

from .. import _api
from .._solve import solve_in_place, solver_config
from ._core import (
    GRAIN_LENGTH_IN_MINUTES,
    MEDIUM_SCALE,
    ConstraintInfo,
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
    constraint_costs,
    constraint_info,
    demo_data,  # the name before 0.5.1
    generate_demo_data,
    list_constraints,
)
from .models import MeetingScheduleModel, solve_request, with_initial_solution

__all__ = [
    "GRAIN_LENGTH_IN_MINUTES",
    "MEDIUM_SCALE",
    "ConstraintInfo",
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
    "constraint_costs",
    "constraint_info",
    "generate_demo_data",
    "list_constraints",
    "solve",
    "solve_request",
    "with_initial_solution",
]


# solve() returns what it was given: the compiled problem, or its pydantic model.
_Problem = TypeVar("_Problem")


@_api.model_solve("meeting-scheduling", MeetingScheduleModel)
def solve(
    schedule: _Problem,
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
    """High-level API that solves a schedule and returns it.

    ``schedule`` is a `_core.MeetingSchedule` (solved in place and returned)
    or a `models.MeetingScheduleModel` (converted, solved, and returned as a new model; the
    one passed in is left as it was).

    ``config`` becomes the base settings (None = engine defaults: 30 s budget);
    explicit keyword arguments override config. ``stop_at_first_feasible``
    stops as soon as hard 0 is reached (best_score_limit=(0, -inf)).

    ``parallel_solver_count`` runs that many independent starts, start i with
    ``random_seed + i``, and returns the best one. ``on_new_best`` reports the first start
    only, so the result can be better than the last score it was called with. With
    ``constraints`` a solve runs one start.

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function directly
    (see `optimaru.meeting_scheduling.constraints`). Only the listed constraints are scored;
    None uses all twelve default constraints::

        from optimaru.meeting_scheduling.constraints import get_constraints
        ms.solve(schedule, constraints=get_constraints())
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
    if isinstance(schedule, MeetingSchedule):
        solve_in_place(Solver(cfg), schedule, constraints)
        return schedule
    if isinstance(schedule, MeetingScheduleModel):
        core = schedule.to_core()
        solve_in_place(Solver(cfg), core, constraints)
        return MeetingScheduleModel.from_core(core)
    raise TypeError(f"schedule must be a MeetingSchedule or a MeetingScheduleModel: {type(schedule)!r}")

"""Conference scheduling — Python wrapper of the C++ solver on the optimaru engine.

`Timeslot`/`Room`/`Speaker`/`Talk`/`ConferenceSchedule`/`Score`/`Solver`/
`SolverConfig`/`calculate_score`/`constraint_costs`/`constraint_info` come from the
compiled C++ core (`_core`). `Talk.slot_room` is the decision variable
(-1 == unassigned; the value is the composite encoding timeslot_index * room_count + room_index),
and solve changes it and `score` in place::

    import optimaru.conference_scheduling as cs

    schedule = cs.demo_conference()
    cs.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(schedule.score, schedule.assignment())

Score: the 37 constraints at their default weights,
17 hard and 20 soft; three of the soft ones (`*_diversity_cost`) are
rewards. `constraint_costs(schedule)` gives every constraint's raw cost (the only
per-constraint surface), and
`constraint_info()` lists every constraint with its default level and weight. Pick or
re-weight constraints with `solve(..., constraints=...)` (see
`optimaru.conference_scheduling.constraints`).

The pydantic mirror (for serialization) is in `optimaru.conference_scheduling.models`.
"""

from typing import TypeVar

from .._solve import solve_in_place, solver_config
from ._core import (
    ConferenceSchedule,
    ConstraintInfo,
    Room,
    Score,
    Solver,
    SolverConfig,
    Speaker,
    Talk,
    Timeslot,
    calculate_score,
    constraint_costs,
    constraint_info,
    list_constraints,
    prepare,
)
from .demo import demo_conference
from .models import ConferenceScheduleModel, solve_request, with_initial_solution

__all__ = [
    "ConferenceSchedule",
    "ConstraintInfo",
    "Room",
    "Score",
    "Solver",
    "SolverConfig",
    "Speaker",
    "Talk",
    "Timeslot",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "demo_conference",
    "list_constraints",
    "prepare",
    "solve",
    "solve_request",
    "with_initial_solution",
]


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
    config: SolverConfig | None = None,
    constraints=None,
) -> _Problem:
    """High-level API that solves a schedule and returns it in the same shape.

    ``schedule`` is a `ConferenceSchedule` or a `models.ConferenceScheduleModel` (converted, solved, and
    returned as a new model; the one passed in is left as it was).

    ``config`` becomes the base settings (`_core.SolverConfig`, or an object with
    `to_core()` — `models.SolveConfig`). None = engine defaults (30 s budget).
    Explicit keyword arguments override config.

    ``stop_at_first_feasible`` stops as soon as hard 0 is reached
    (best_score_limit=(0, -inf)).

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function directly (see
    `optimaru.conference_scheduling.constraints`). Only the listed constraints are scored;
    None uses all 37 default constraints at their default weights::

        from optimaru.conference_scheduling.constraints import get_constraints
        cs.solve(schedule, constraints=get_constraints())
    """
    if config is not None and not isinstance(config, SolverConfig):
        if not hasattr(config, "to_core"):
            raise ValueError("config must be a SolverConfig or an object with to_core()")
        config = config.to_core()  # a models.SolveConfig
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
    if isinstance(schedule, ConferenceSchedule):
        solve_in_place(Solver(cfg), schedule, constraints)
        return schedule
    if isinstance(schedule, ConferenceScheduleModel):
        core = schedule.to_core()
        solve_in_place(Solver(cfg), core, constraints)
        return ConferenceScheduleModel.from_core(core)
    raise TypeError(f"schedule must be a ConferenceSchedule or a models.ConferenceScheduleModel: {type(schedule)!r}")

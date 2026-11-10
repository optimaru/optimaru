"""Tournament scheduling — Python wrapper of the C++ solver on the optimaru engine.

Assigns teams to daily match slots (TeamAssignment). Four constraints
(2 hard + 1 medium + 1 soft; the three score levels are folded as
``soft = -(medium * MEDIUM_SCALE + raw soft)``, MEDIUM_SCALE == 1e6; for the details see the
models/_core docs). ``constraint_info()`` lists each constraint's default level and weight
(``assignment_count_imbalance_cost`` is medium at weight 1, in medium units);
``constraint_costs(schedule)`` gives the raw per-constraint costs::

    import optimaru.tournament_scheduling as ts
    from optimaru.tournament_scheduling.models import demo_data

    schedule = demo_data()  # the demo instance (7 teams, 18 days x 4 slots)
    result = ts.solve(schedule, time_limit_seconds=5.0, random_seed=42)
    print(result.score_hard, result.score_soft)

`solve` accepts both the pydantic mirror (`models.TournamentScheduleModel`) and `_core`
objects (`TournamentSchedule`). The pydantic mirror (for serialization) is in
`optimaru.tournament_scheduling.models`.
"""

from typing import TypeVar

from .._solve import solve_in_place, solver_config
from ._core import (
    MEDIUM_SCALE,
    ConstraintInfo,
    Day,
    Score,
    Solver,
    SolverConfig,
    Team,
    TeamAssignment,
    TournamentSchedule,
    UnavailabilityPenalty,
    calculate_score,
    constraint_costs,
    constraint_info,
    list_constraints,
    unfairness,
)
from .models import TournamentScheduleModel, with_initial_solution

__all__ = [
    "MEDIUM_SCALE",
    "ConstraintInfo",
    "Day",
    "Score",
    "Solver",
    "SolverConfig",
    "Team",
    "TeamAssignment",
    "TournamentSchedule",
    "UnavailabilityPenalty",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "list_constraints",
    "solve",
    "unfairness",
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
    """High-level API that solves a schedule and returns it.

    ``schedule`` is a `_core.TournamentSchedule` (solved in place and returned)
    or a `models.TournamentScheduleModel` (converted, solved, and returned as a new model; the
    one passed in is left as it was).

    ``config`` becomes the base settings (None = engine defaults: 30 s budget);
    explicit keyword arguments override config. ``stop_at_first_feasible``
    stops as soon as hard 0 is reached (best_score_limit=(0, -inf)).

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function directly
    (see `optimaru.tournament_scheduling.constraints`). Only the listed constraints are scored;
    None uses all four default constraints::

        from optimaru.tournament_scheduling.constraints import get_constraints
        ts.solve(schedule, constraints=get_constraints())
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
    if isinstance(schedule, TournamentSchedule):
        solve_in_place(Solver(cfg), schedule, constraints)
        return schedule
    if isinstance(schedule, TournamentScheduleModel):
        core = schedule.to_core()
        solve_in_place(Solver(cfg), core, constraints)
        return TournamentScheduleModel.from_core(core)
    raise TypeError(f"schedule must be a TournamentSchedule or a TournamentScheduleModel: {type(schedule)!r}")

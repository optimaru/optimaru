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
`optimaru.tournament_scheduling.models` (optional dependency).
"""

import copy

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
    "solve",
    "unfairness",
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
    schedule,
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
    """High-level API that solves a schedule and returns it.

    ``schedule`` is a `_core.TournamentSchedule` (solved in place and returned)
    or a `models.TournamentScheduleModel` (converted, solved, returned as a new model).

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

    def _solve(core):
        if constraints is None:
            Solver(cfg).solve(core)
        else:
            Solver(cfg).solve_with_constraints(core, constraints)

    if isinstance(schedule, TournamentSchedule):
        _solve(schedule)
        return schedule

    # pydantic mirror (lazy import: optional dependency).
    from .models import TournamentScheduleModel

    if isinstance(schedule, TournamentScheduleModel):
        core = schedule.to_core()
        _solve(core)
        return TournamentScheduleModel.from_core(core)
    raise TypeError(f"schedule must be a TournamentSchedule or a TournamentScheduleModel: {type(schedule)!r}")

"""Sports league scheduling — Python wrapper of the C++ solver on the optimaru engine.

`Team`/`Round`/`Match`/`LeagueSchedule`/`Score`/`Breakdown`/`Solver`/
`SolverConfig`/`calculate_score`/`compute_breakdown` come from the compiled C++ core
(`_core`). `Match.round` is the planning variable (-1 == unassigned; the value is a
position in `LeagueSchedule.rounds`), and solve changes it and `score`
in place::

    import sports_league as sl

    schedule = sl.demo_schedule()
    sl.solve(schedule, time_limit_seconds=10.0, random_seed=42,
             stop_at_first_feasible=True)
    print(schedule.score, schedule.assignment())

The pydantic mirror (for serialization) is in `sports_league.models` (optional dependency).
"""

from ._core import (
    Breakdown,
    LeagueSchedule,
    Match,
    Round,
    Score,
    Solver,
    SolverConfig,
    Team,
    calculate_score,
    compute_breakdown,
    constraint_costs,
    list_constraints,
)
from .demo import demo_schedule

__all__ = [
    "Breakdown",
    "LeagueSchedule",
    "Match",
    "Round",
    "Score",
    "Solver",
    "SolverConfig",
    "Team",
    "calculate_score",
    "compute_breakdown",
    "list_constraints",
    "constraint_costs",
    "demo_schedule",
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
    schedule: LeagueSchedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> LeagueSchedule:
    """High-level API that solves a `LeagueSchedule` in place and returns it.

    ``config`` becomes the base settings (`_core.SolverConfig`, or an object with
    `to_core()` — `models.SolveConfig`). None = engine defaults (30 s budget).
    Explicit keyword arguments override config.

    ``stop_at_first_feasible`` stops as soon as hard 0 is reached
    (best_score_limit=(0, -1e18)).

    ``constraints`` picks the cost function directly
    (see `sports_league.constraints`). Only the listed constraints are scored; None
    uses all ten default constraints::

        from sports_league.constraints import get_constraints
        sls.solve(schedule, constraints=get_constraints())
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

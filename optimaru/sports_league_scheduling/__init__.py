"""Sports league scheduling — Python wrapper of the C++ solver on the optimaru engine.

`Team`/`Round`/`Match`/`LeagueSchedule`/`Score`/`Solver`/`SolverConfig`/
`calculate_score`/`constraint_costs`/`constraint_info` come from the compiled C++ core
(`_core`). `Match.round` is the decision variable (-1 == unassigned; the value is a
position in `LeagueSchedule.rounds`), and solve changes it and `score`
in place::

    import optimaru.sports_league_scheduling as sl

    schedule = sl.demo_schedule()
    sl.solve(schedule, time_limit_seconds=10.0, random_seed=42,
             stop_at_first_feasible=True)
    print(schedule.score, schedule.assignment())

Score, at the default weights: hard = -(team_conflict_cost +
consecutive_home_matches_cost + consecutive_away_matches_cost +
repeat_match_on_next_day_cost), soft = -(the five *_hop_cost travel terms in km +
1000 x workday_classic_matches_cost). `constraint_costs(schedule)` gives the raw cost of
every constraint (the only per-constraint surface); `constraint_info()`
lists every constraint with its default level and weight. Change a weight or a level
through a constraint selection (see `optimaru.sports_league_scheduling.constraints`).

The pydantic mirror (for serialization) is in `optimaru.sports_league_scheduling.models` (optional dependency).
"""

import copy
from typing import TypeVar

from ._core import (
    ConstraintInfo,
    LeagueSchedule,
    Match,
    Round,
    Score,
    Solver,
    SolverConfig,
    Team,
    calculate_score,
    constraint_costs,
    constraint_info,
    list_constraints,
)
from .demo import demo_schedule

__all__ = [
    "ConstraintInfo",
    "LeagueSchedule",
    "Match",
    "Round",
    "Score",
    "Solver",
    "SolverConfig",
    "Team",
    "calculate_score",
    "list_constraints",
    "constraint_costs",
    "constraint_info",
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
    """High-level API that solves a schedule in place and returns it.

    ``schedule`` is a `LeagueSchedule` or a `models.LeagueScheduleModel` (converted, solved, and
    returned as a new model; the one passed in is left as it was).

    ``config`` becomes the base settings (`_core.SolverConfig`, or an object with
    `to_core()` — `models.SolveConfig`). None = engine defaults (30 s budget).
    Explicit keyword arguments override config.

    ``stop_at_first_feasible`` stops as soon as hard 0 is reached
    (best_score_limit=(0, -inf)).

    ``full_assert`` re-verifies every incremental score against a full recompute
    (a debugging aid; much slower).

    ``constraints`` picks the cost function directly
    (see `optimaru.sports_league_scheduling.constraints`). Only the listed constraints are scored, a name
    picked several times once per pick; None uses all ten default constraints at their
    default weights::

        from optimaru.sports_league_scheduling.constraints import get_constraints
        sl.solve(schedule, constraints=get_constraints())
    """
    if config is None:
        cfg = SolverConfig()
    elif isinstance(config, SolverConfig):
        cfg = copy.copy(config)  # the overrides below must not leak into it
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

    if isinstance(schedule, LeagueSchedule):
        _solve(schedule)
        return schedule

    # pydantic model (lazy import: pydantic is an optional dependency).
    from .models import LeagueScheduleModel

    if isinstance(schedule, LeagueScheduleModel):
        core = schedule.to_core()
        _solve(core)
        return LeagueScheduleModel.from_core(core)
    raise TypeError(f"schedule must be a LeagueSchedule or a models.LeagueScheduleModel: {type(schedule)!r}")

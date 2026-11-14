"""Sports league scheduling — Python wrapper of the C++ solver on the optimaru engine.

`Team`/`Round`/`Match`/`LeagueSchedule`/`Score`/`Solver`/`SolverConfig`/
`calculate_score`/`constraint_costs`/`constraint_info` come from the compiled C++ core
(`_core`). `Match.round` is the decision variable (-1 == unassigned; the value is a
position in `LeagueSchedule.rounds`), and solve changes it and `score`
in place::

    import optimaru.sports_league_scheduling as sl

    schedule = sl.generate_demo_data()
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

The pydantic mirror (for serialization) is in `optimaru.sports_league_scheduling.models`.
"""

from typing import TypeVar

from .. import _api
from .._solve import solve_in_place, solver_config
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
from .demo import demo_schedule, generate_demo_data  # demo_schedule: the name before 0.5.1
from .models import LeagueScheduleModel, solve_request, with_initial_solution

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


@_api.model_solve("sports-league-scheduling", LeagueScheduleModel)
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
    if isinstance(schedule, LeagueSchedule):
        solve_in_place(Solver(cfg), schedule, constraints)
        return schedule
    if isinstance(schedule, LeagueScheduleModel):
        core = schedule.to_core()
        solve_in_place(Solver(cfg), core, constraints)
        return LeagueScheduleModel.from_core(core)
    raise TypeError(f"schedule must be a LeagueSchedule or a models.LeagueScheduleModel: {type(schedule)!r}")

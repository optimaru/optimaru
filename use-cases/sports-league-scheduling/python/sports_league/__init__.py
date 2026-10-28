"""Sports league scheduling — Timefold engine 기반 C++ 솔버의 Python 래퍼.

`Team`/`Round`/`Match`/`LeagueSchedule`/`Score`/`Breakdown`/`Solver`/
`SolverConfig`/`calculate_score`/`compute_breakdown` 는 컴파일된 C++ 코어
(`_core`)에서 온다. `Match.round` 가 planning variable (-1 == 미배정,
값은 `LeagueSchedule.rounds` 의 위치)이고, solve 는 그것과 `score` 를
in place 로 바꾼다::

    import sports_league as sl

    schedule = sl.demo_schedule()
    sl.solve(schedule, time_limit_seconds=10.0, random_seed=42,
             stop_at_first_feasible=True)
    print(schedule.score, schedule.assignment())

pydantic 미러(직렬화용)는 `sports_league.models` 에 있다 (선택 의존성).
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
]


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
    """`LeagueSchedule` 을 in place 로 풀어 그대로 돌려주는 고수준 API.

    ``config`` 가 기본 설정이 된다 (`_core.SolverConfig`, 또는 `to_core()` 를
    가진 객체 — `models.SolveConfig`). None 이면 엔진 기본값(30초 예산).
    명시한 키워드 인자가 config 위에 덮어쓴다.

    ``stop_at_first_feasible`` 은 hard 0 에 도달하는 즉시 종료한다
    (best_score_limit=(0, -1e18)).

    ``constraints`` 로 비용 함수를 직접 고를 수 있다
    (`sports_league.constraints` 참고). 준 목록의 제약만 채점되고, None
    이면 기본 열 제약 전부를 쓴다::

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
        raise ValueError("config 는 SolverConfig 또는 to_core() 를 가진 객체여야 한다")
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

"""Tournament scheduling — Timefold engine 기반 C++ 솔버의 Python 래퍼.

Java quickstart "tournament-scheduling" 의 포팅이다: 팀을 일자별 경기 슬롯
(TeamAssignment)에 배정한다. 제약은 Java ConstraintProvider 와 동일 의미론
(하드 2 + medium 1 + soft 1; 점수 매핑은 models/_core 문서 참고)::

    import tournament_scheduling as ts
    from tournament_scheduling.models import demo_data

    schedule = demo_data()  # Java DemoDataGenerator 재현 (팀 7, 18일 x 4슬롯)
    result = ts.solve(schedule, time_limit_seconds=5.0, random_seed=42)
    print(result.score_hard, result.score_soft)

`solve` 는 pydantic 미러(`models.TournamentScheduleModel`)와 `_core`
객체(`TournamentSchedule`) 둘 다 받는다. pydantic 미러(직렬화용)는
`tournament_scheduling.models` 에 있다 (선택 의존성).
"""

from ._core import (
    MEDIUM_SCALE,
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
    list_constraints,
    unfairness,
)

__all__ = [
    "MEDIUM_SCALE",
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
]


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
    """스케줄을 풀어 돌려주는 고수준 API.

    ``schedule`` 은 `_core.TournamentSchedule` (in place 로 풀고 그대로 반환)
    또는 `models.TournamentScheduleModel` (변환해 풀고 새 모델로 반환).

    ``config`` 가 기본 설정이 된다 (None 이면 엔진 기본값: 30초 예산);
    명시한 키워드 인자가 config 위에 덮어쓴다. ``stop_at_first_feasible``
    은 hard 0 도달 즉시 종료한다 (best_score_limit=(0, -1e18)).

    ``constraints`` 로 비용 함수를 직접 고를 수 있다
    (`tournament_scheduling.constraints` 참고). 준 목록의 제약만 채점되고,
    None 이면 기본 네 제약 전부를 쓴다::

        from tournament_scheduling.constraints import get_constraints
        ts.solve(schedule, constraints=get_constraints())
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

    if isinstance(schedule, TournamentSchedule):
        _solve(schedule)
        return schedule

    # pydantic 미러 (선택 의존성이라 지연 임포트).
    from .models import TournamentScheduleModel

    if isinstance(schedule, TournamentScheduleModel):
        core = schedule.to_core()
        _solve(core)
        return TournamentScheduleModel.from_core(core)
    raise TypeError(f"schedule 은 TournamentSchedule 또는 TournamentScheduleModel 이어야 한다: {type(schedule)!r}")

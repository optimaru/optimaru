"""Meeting scheduling — Timefold engine 기반 C++ 솔버의 Python 래퍼.

Java quickstart "meeting-scheduling" 의 포팅이다: 미팅에 시작 타임그레인
(15분 단위)과 회의실을 배정한다. Java 엔티티의 planning variable 두 개
(startingTimeGrain, room)는 합성 인코딩 ``grain_room = time_grain_pos *
room_count + room_index`` 로 하나의 BASIC 변수가 된다 (-1 == 미배정).
제약은 Java ConstraintProvider 와 동일 의미론 (하드 5 + medium 2 + soft 5;
HardMediumSoftScore 는 ``soft = -(medium * MEDIUM_SCALE + raw soft)`` 로
접는다, MEDIUM_SCALE == 1e6)::

    import meeting_scheduling as ms

    schedule = ms.demo_data()  # Java DemoDataGenerator 재현 (미팅 24개)
    result = ms.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(result.score.hard, result.score.soft)

`solve` 는 `_core.MeetingSchedule` (in place 로 풀고 그대로 반환)과 pydantic
미러 `models.MeetingScheduleModel` (변환해 풀고 새 모델로 반환) 둘 다 받는다.
pydantic 미러(직렬화용)는 `meeting_scheduling.models` 에 있다 (선택 의존성).
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

    ``schedule`` 은 `_core.MeetingSchedule` (in place 로 풀고 그대로 반환)
    또는 `models.MeetingScheduleModel` (변환해 풀고 새 모델로 반환).

    ``config`` 가 기본 설정이 된다 (None 이면 엔진 기본값: 30초 예산);
    명시한 키워드 인자가 config 위에 덮어쓴다. ``stop_at_first_feasible``
    은 hard 0 도달 즉시 종료한다 (best_score_limit=(0, -1e18)).

    ``constraints`` 로 비용 함수를 직접 고를 수 있다
    (`meeting_scheduling.constraints` 참고). 준 목록의 제약만 채점되고,
    None 이면 기본 열두 제약 전부를 쓴다::

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

    # pydantic 미러 (선택 의존성이라 지연 임포트).
    from .models import MeetingScheduleModel

    if isinstance(schedule, MeetingScheduleModel):
        core = schedule.to_core()
        _solve(core)
        return MeetingScheduleModel.from_core(core)
    raise TypeError(f"schedule 은 MeetingSchedule 또는 MeetingScheduleModel 이어야 한다: {type(schedule)!r}")

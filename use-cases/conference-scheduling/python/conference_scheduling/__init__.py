"""Conference scheduling — Timefold engine 기반 C++ 솔버의 Python 래퍼.

`Timeslot`/`Room`/`Speaker`/`Talk`/`ConferenceSchedule`/`Score`/`Breakdown`/
`Solver`/`SolverConfig`/`calculate_score`/`compute_breakdown` 는 컴파일된
C++ 코어(`_core`)에서 온다. `Talk.slot_room` 이 planning variable
(-1 == 미배정, 값은 timeslot_index * room_count + room_index 합성 인코딩)이고,
solve 는 그것과 `score` 를 in place 로 바꾼다::

    import conference_scheduling as cs

    schedule = cs.demo_conference()
    cs.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(schedule.score, schedule.assignment())

pydantic 미러(직렬화용)는 `conference_scheduling.models` 에 있다
(선택 의존성).
"""

from ._core import (
    Breakdown,
    ConferenceSchedule,
    Room,
    Score,
    Solver,
    SolverConfig,
    Speaker,
    Talk,
    Timeslot,
    calculate_score,
    compute_breakdown,
    constraint_costs,
    list_constraints,
    prepare,
)
from .demo import demo_conference

__all__ = [
    "Breakdown",
    "ConferenceSchedule",
    "Room",
    "Score",
    "Solver",
    "SolverConfig",
    "Speaker",
    "Talk",
    "Timeslot",
    "calculate_score",
    "compute_breakdown",
    "list_constraints",
    "constraint_costs",
    "demo_conference",
    "prepare",
    "solve",
]


def solve(
    schedule: ConferenceSchedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> ConferenceSchedule:
    """`ConferenceSchedule` 을 in place 로 풀어 그대로 돌려주는 고수준 API.

    ``config`` 가 기본 설정이 된다 (`_core.SolverConfig`, 또는 `to_core()` 를
    가진 객체 — `models.SolveConfig`). None 이면 엔진 기본값(30초 예산).
    명시한 키워드 인자가 config 위에 덮어쓴다.

    ``stop_at_first_feasible`` 은 hard 0 에 도달하는 즉시 종료한다
    (best_score_limit=(0, -1e18)).
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

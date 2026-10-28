"""Maintenance scheduling — Timefold engine 기반 C++ 솔버의 Python 래퍼.

Timefold quickstart `maintenance-scheduling` 의 포트다: 유지보수 작업(Job)에
crew 와 시작일(주말 제외 TimeGrain)을 배정한다. Java 도메인의 planning
variable 2개(crew, startDate)는 엔진의 BASIC 변수 1개로 합성 인코딩된다::

    crew_date = start_date_index * crew_count + crew_index   # -1 == 미배정

날짜는 epoch day(1970-01-01 기준 일수, Java LocalDate.toEpochDay)로
주고받는다. pydantic 미러(`maintenance_scheduling.models`, 선택 의존성)가
`datetime.date` 와의 변환을 맡는다.

제약 (HardSoftScore — Java ConstraintProvider 와 동일 의미론):
  hard 1. crewConflict        같은 crew 의 겹치는 작업쌍마다 -겹친 일수
  hard 2. minStartDate        minStartDate 보다 일찍 시작한 일수만큼 -1
  hard 3. maxEndDate          maxEndDate 보다 늦게 끝난 일수만큼 -1
  soft 4. beforeIdealEndDate  idealEndDate 보다 일찍 끝난 일수만큼 -1
  soft 5. afterIdealEndDate   idealEndDate 보다 늦게 끝난 일수만큼 -1'000'000
  soft 6. tagConflict         겹치는 작업쌍의 공유 태그 x 겹친 일수만큼 -1'000

사용::

    import maintenance_scheduling as mnt

    schedule = mnt.generate_demo_data("SMALL")
    mnt.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(schedule.score, schedule.score.is_feasible())
"""

from ._core import (
    Crew,
    Job,
    MaintenanceSchedule,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    generate_demo_data,
    list_constraints,
    plus_business_days,
)

__all__ = [
    "Crew",
    "Job",
    "MaintenanceSchedule",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "generate_demo_data",
    "list_constraints",
    "constraint_costs",
    "plus_business_days",
    "solve",
]


def solve(
    schedule: MaintenanceSchedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> MaintenanceSchedule:
    """`MaintenanceSchedule` 을 in place 로 풀고 그대로 돌려주는 고수준 API.

    ``config`` 가 기본 설정이 된다 (None 이면 엔진 기본값: 30초 예산, ALNS
    켜짐). 명시한 키워드 인자가 config 위에 덮어쓴다.
    ``stop_at_first_feasible`` 은 hard 0 에 도달하는 즉시 종료한다
    (best_score_limit=(0, -1e18)).

    ``constraints`` 로 비용 함수를 직접 고를 수 있다
    (`maintenance_scheduling.constraints` 참고). 준 목록의 제약만 채점되고,
    None 이면 기본 여섯 제약 전부를 쓴다::

        from maintenance_scheduling.constraints import get_constraints
        mnt.solve(schedule, constraints=get_constraints())
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
    if constraints is None:
        Solver(cfg).solve(schedule)
    else:
        Solver(cfg).solve_with_constraints(schedule, constraints)
    return schedule

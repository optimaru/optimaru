"""Project job scheduling (다중 프로젝트 RCPSP) — Timefold engine 기반 C++
솔버의 Python 래퍼.

`Project`/`Resource`/`ResourceRequirement`/`ExecutionMode`/`Job`/`Allocation`/
`ProjectJobSchedule`/`JobType`/`Score`/`Solver`/`SolverConfig` 와
`calculate_score` 는 컴파일된 C++ 코어(`_core`)에서 온다.

Java quickstart 의 planning variable 2개 (executionMode, delay∈[0, 500)) 는
합성 인코딩된 BASIC 변수 `Allocation.mode_delay` 하나로 포팅됐다::

    -1                                  미초기화 (자원 사용 없음,
                                        날짜는 delay 0/duration 0 통과)
    delay * max_mode_count + mode_slot  (실제 mode index = mode_slot %
                                        해당 job 의 mode 수)

Java 의 shadow variable 체인 (predecessorsDoneDate → startDate → endDate)
은 파생 상태로 재현된다: `calculate_score` / `Solver.solve` 가 위상 정렬
순서로 재계산해 `Allocation.start_date`/`end_date` 에 채운다. SOURCE/SINK
할당은 항상 pinned (Java 의 @PlanningPin + setPinned 그대로) 이고, 풀기 전에
미초기화 상태면 (첫 mode, delay 0) 으로 고정된다.

점수 규약 (HardMediumSoftScore, Java quickstart 그대로): hard 는 자원 용량
위반 (0 == feasible; non-renewable 은 전체 합, renewable 은 일자별), medium
은 프로젝트별 지연(sink 종료 − critical path 종료, 양수만)의 합, soft 는
makespan (최대 sink 종료일). 사전식 비교(hard 우선). 엔진 내부적으로는
HardSoftScore(int64, double)에 `engine.soft = medium * 10**9 + soft` 로
매핑된다 (레벨 순서 보존).

고수준 진입점은 `solve` 다::

    import project_job as pj
    from project_job.demo import generate_demo_data

    schedule = generate_demo_data()                 # models.ProjectJobScheduleModel
    result = pj.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(result.score_hard, result.score_medium, result.score_soft)

pydantic 미러(직렬화용)는 `project_job.models` 에 있다 (선택 의존성).
"""

from ._core import (
    Allocation,
    ExecutionMode,
    Job,
    JobType,
    Project,
    ProjectJobSchedule,
    Resource,
    ResourceRequirement,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    list_constraints,
)

__all__ = [
    "Allocation",
    "ExecutionMode",
    "Job",
    "JobType",
    "Project",
    "ProjectJobSchedule",
    "Resource",
    "ResourceRequirement",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "solve",
    "list_constraints",
    "constraint_costs",
]


def _build_config(
    *,
    time_limit_seconds: float | None,
    random_seed: int | None,
    parallel_solver_count: int | None,
    stop_at_first_feasible: bool,
    full_assert: bool,
    on_new_best,
    config,
) -> SolverConfig:
    cfg = config if config is not None else SolverConfig()
    if time_limit_seconds is not None:
        cfg.spent_limit_seconds = time_limit_seconds
    if random_seed is not None:
        cfg.random_seed = random_seed
    if parallel_solver_count is not None:
        cfg.parallel_solver_count = parallel_solver_count
    if stop_at_first_feasible:
        # hard 0 도달 즉시 종료 (medium/soft 는 사실상 -inf 하한).
        cfg.best_score_limit = (0, -(2**62), -(2**62))
    if full_assert:
        cfg.full_assert = True
    if on_new_best is not None:
        cfg.on_new_best = on_new_best
    return cfg


def solve(
    schedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config=None,
    constraints=None,
):
    """프로젝트 잡 스케줄링 문제를 풀어 같은 형태로 돌려주는 고수준 API.

    ``schedule`` 은 `_core.ProjectJobSchedule` (in place 로 풀고 그대로
    반환) 또는 `models.ProjectJobScheduleModel` (엔진 형태로 변환해 풀고
    결과를 새 `ProjectJobScheduleModel` 로 반환; pydantic 필요) 를 받는다.

    ``config`` 가 기본 설정이 된다 (`SolverConfig`; None 이면 엔진 기본값
    30초 예산). 명시한 키워드 인자가 config 위에 덮어쓴다.
    ``stop_at_first_feasible`` 은 hard 0 에 도달하는 즉시 종료한다.
    ``on_new_best`` 는 새 best 마다 ``(step, Score)`` 로 불린다.

    ``constraints`` 로 비용 함수를 직접 고를 수 있다
    (`project_job.constraints` 참고). 준 목록의 제약만 채점되고, None 이면
    기본 네 제약 전부를 쓴다::

        from project_job.constraints import get_constraints
        pjs.solve(schedule, constraints=get_constraints())
    """
    cfg = _build_config(
        time_limit_seconds=time_limit_seconds,
        random_seed=random_seed,
        parallel_solver_count=parallel_solver_count,
        stop_at_first_feasible=stop_at_first_feasible,
        full_assert=full_assert,
        on_new_best=on_new_best,
        config=config,
    )

    def _solve(core_schedule):
        if constraints is None:
            Solver(cfg).solve(core_schedule)
        else:
            Solver(cfg).solve_with_constraints(core_schedule, constraints)

    if isinstance(schedule, ProjectJobSchedule):
        _solve(schedule)
        return schedule

    # pydantic 모델 경로 (지연 임포트: pydantic 은 선택 의존성).
    from .models import ProjectJobScheduleModel

    if not isinstance(schedule, ProjectJobScheduleModel):
        raise TypeError(
            f"schedule 은 ProjectJobSchedule 또는 models.ProjectJobScheduleModel 이어야 한다: {type(schedule)!r}"
        )
    core_schedule = schedule.to_core()
    _solve(core_schedule)
    return ProjectJobScheduleModel.from_core(core_schedule)

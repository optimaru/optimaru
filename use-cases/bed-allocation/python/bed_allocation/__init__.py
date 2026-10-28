"""Bed allocation (병상 배정) — Timefold engine 기반 C++ 솔버의 Python 래퍼.

`Department`/`Room`/`Bed`/`Stay`/`BedPlan`/`Score`/`Solver`/`SolverConfig` 와
`calculate_score` 는 컴파일된 C++ 코어(`_core`)에서 온다. `Stay.bed` 가
planning variable 이다::

    -1                          미초기화 (점수 기여 없음)
    0..bed_count()-1            실제 병상 인덱스
    bed_count() (no_bed_value)  "no bed" — quickstart 의 null 병상
                                (allowsUnassigned; medium 레벨로 벌점)

점수 규약 (HardMediumSoftScore, Java quickstart 그대로): hard 는 병상/성별/
연령/필수장비 위반 (0 == feasible), medium 은 미배정 stay 의 박수 합,
soft 는 선호(방 크기/전문과목/선호장비) 벌점. 사전식 비교(hard 우선).
엔진 내부적으로는 HardSoftScore(int64, double)에
`engine.soft = medium * 10**9 + soft` 로 매핑된다 (레벨 순서 보존).

고수준 진입점은 `solve` 다::

    import bed_allocation as ba
    from bed_allocation.demo import generate_demo_data

    plan = generate_demo_data()                     # models.BedPlanModel
    result = ba.solve(plan, time_limit_seconds=10.0, random_seed=42)
    print(result.score_hard, result.score_medium, result.score_soft)

pydantic 미러(직렬화용)는 `bed_allocation.models` 에 있다 (선택 의존성).
"""

from ._core import (
    Bed,
    BedPlan,
    Department,
    Gender,
    GenderLimitation,
    Room,
    Score,
    Solver,
    SolverConfig,
    Stay,
    calculate_score,
    constraint_costs,
    list_constraints,
)

__all__ = [
    "Bed",
    "BedPlan",
    "Department",
    "Gender",
    "GenderLimitation",
    "Room",
    "Score",
    "Solver",
    "SolverConfig",
    "Stay",
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
    plan,
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
    """병상 배정 문제를 풀어 같은 형태로 돌려주는 고수준 API.

    ``plan`` 은 `_core.BedPlan` (in place 로 풀고 그대로 반환) 또는
    `models.BedPlanModel` (엔진 형태로 변환해 풀고 결과를 새
    `BedPlanModel` 로 반환; pydantic 필요) 를 받는다.

    ``config`` 가 기본 설정이 된다 (`SolverConfig`; None 이면 엔진 기본값
    30초 예산). 명시한 키워드 인자가 config 위에 덮어쓴다.
    ``stop_at_first_feasible`` 은 hard 0 에 도달하는 즉시 종료한다.
    ``on_new_best`` 는 새 best 마다 ``(step, Score)`` 로 불린다.

    ``constraints`` 로 비용 함수를 직접 고를 수 있다
    (`bed_allocation.constraints` 참고). 준 목록의 제약만 채점되고,
    None 이면 기본 열두 제약 전부를 쓴다::

        from bed_allocation.constraints import get_constraints
        ba.solve(plan, constraints=get_constraints())
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

    def _solve(core_plan):
        if constraints is None:
            Solver(cfg).solve(core_plan)
        else:
            Solver(cfg).solve_with_constraints(core_plan, constraints)

    if isinstance(plan, BedPlan):
        _solve(plan)
        return plan

    # pydantic 모델 경로 (지연 임포트: pydantic 은 선택 의존성).
    from .models import BedPlanModel

    if not isinstance(plan, BedPlanModel):
        raise TypeError(f"plan 은 BedPlan 또는 models.BedPlanModel 이어야 한다: {type(plan)!r}")
    core_plan = plan.to_core()
    _solve(core_plan)
    return BedPlanModel.from_core(core_plan)

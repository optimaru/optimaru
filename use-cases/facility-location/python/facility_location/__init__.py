"""Facility location — Timefold engine 기반 C++ 솔버의 Python 래퍼.

Timefold quickstart facility-location 포팅: 수요처(Consumer)마다 시설
(Facility) 하나를 고르는 BASIC planning variable (`Consumer.facility`,
시설 인덱스, -1 = 미배정) 하나로 푼다.

Score (HardSoftScore):
  hard = -(사용 시설별 max(0, 배정 수요 - 용량) 합)          # 시설 용량
  soft = -(2 x 사용 시설 설치비 합 + 6 x 수요처-시설 거리 합)  # 설치비 + 거리
거리 가중치 6 은 Java 솔루션의 ConstraintWeightOverrides 값이다 (provider
기본은 5); `problem.distance_from_facility_weight` 로 바꿀 수 있다.

기본 사용::

    import facility_location as fl

    problem = fl.FacilityLocationProblem(facilities, consumers)
    fl.solve(problem, time_limit_seconds=5.0, random_seed=42)
    print(problem.score, problem.total_cost(), problem.total_distance_meters())

pydantic 미러(직렬화용)는 `facility_location.models` 에 있다 (선택 의존성).
"""

from ._core import (
    Consumer,
    Facility,
    FacilityLocationProblem,
    Location,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    list_constraints,
)

__all__ = [
    "Consumer",
    "Facility",
    "FacilityLocationProblem",
    "Location",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "solve",
    "list_constraints",
    "constraint_costs",
]


def solve(
    problem: FacilityLocationProblem,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> Score:
    """`problem` 을 제자리에서 풀고 최종 `Score` 를 돌려주는 고수준 API.

    ``config`` 가 기본 설정이 된다 (None 이면 엔진 기본값: 30초 예산, ALNS
    켜짐). 명시한 키워드 인자가 config 위에 덮어쓴다.
    ``stop_at_first_feasible`` 은 hard 0 에 도달하는 즉시 종료한다.

    ``constraints`` 로 비용 함수를 직접 고를 수 있다
    (`facility_location.constraints` 참고). 준 목록의 제약만 채점되고,
    None 이면 기본 세 제약 전부를 쓴다::

        from facility_location.constraints import get_constraints
        fl.solve(problem, constraints=get_constraints())
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
        Solver(cfg).solve(problem)
    else:
        Solver(cfg).solve_with_constraints(problem, constraints)
    return problem.score

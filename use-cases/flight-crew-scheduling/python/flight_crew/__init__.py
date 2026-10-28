"""Flight crew scheduling — Timefold engine 기반 C++ 솔버의 Python 래퍼.

Timefold quickstart `flight-crew-scheduling` 의 포트다: 항공편의 승무원
좌석(FlightAssignment)마다 승무원(employee)을 고르는 BASIC 변수 문제.

제약 (HardSoftScore, Java ConstraintProvider 의 가중치 그대로):
  hard 1. requiredSkill               스킬 없는 배정마다 -100
  hard 2. flightConflict              같은 승무원의 시간이 겹치는 배정 쌍마다 -10
  hard 3. transferBetweenTwoFlights   출발시각 기준 연속인 두 배정의 공항이
                                      이어지지 않으면(도착공항 != 다음 출발공항)
                                      쌍마다 -1
  hard 4. employeeUnavailability      비행 날짜에 불가한 승무원 배정마다 -10
  soft 5. firstAssignmentNotDepartingFromHome  첫 배정이 홈이 아닌 공항 출발
                                      이면 -1000
  soft 6. lastAssignmentNotArrivingAtHome      마지막 배정이 홈이 아닌 공항
                                      도착이면 -1000

고수준 진입점은 `solve` 다. pydantic 모델(선택 의존성)이나 dict 를 받아
풀린 모델을 돌려준다::

    import flight_crew as fc

    result = fc.solve(problem_dict, time_limit_seconds=10.0, random_seed=42)
    print(result.score_hard, result.score_soft)

엔진 객체(`FlightCrewSchedule`)를 직접 만들어 `Solver` 를 쓰는 저수준
경로도 있다 (pydantic 불필요). pydantic 미러는 `flight_crew.models` 에 있다.
"""

from ._core import (
    Airport,
    Employee,
    Flight,
    FlightAssignment,
    FlightCrewSchedule,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    list_constraints,
)

__all__ = [
    "Airport",
    "Employee",
    "Flight",
    "FlightAssignment",
    "FlightCrewSchedule",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "solve",
    "list_constraints",
    "constraint_costs",
]


def _resolve_config(config) -> SolverConfig:
    if config is None:
        return SolverConfig()
    if isinstance(config, SolverConfig):
        return config
    if hasattr(config, "to_core"):
        return config.to_core()
    raise ValueError("config 는 SolverConfig 또는 to_core() 를 가진 객체(models.SolveConfig)여야 한다")


def solve(
    schedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config=None,
    constraints=None,
):
    """스케줄을 풀어 돌려주는 고수준 API.

    ``schedule`` 은 셋 중 하나다:
      - `models.FlightCrewScheduleModel` (pydantic) -> 풀린 모델을 반환
      - 같은 형태의 dict -> 검증 후 풀린 모델을 반환
      - `_core.FlightCrewSchedule` (엔진 객체) -> in place 로 풀고 그대로 반환

    ``config`` 가 기본 설정이 된다: `_core.SolverConfig` 를 그대로 주거나
    `to_core()` 를 가진 객체(`models.SolveConfig`)를 줄 수 있다. None 이면
    엔진 기본값(30초 예산). 명시한 키워드 인자가 config 위에 덮어쓴다.

    ``stop_at_first_feasible`` 은 hard 0 에 도달하는 즉시 종료한다
    (best_score_limit=(0, -1e18)).

    ``constraints`` 로 비용 함수를 직접 고를 수 있다
    (`flight_crew.constraints` 참고). 준 목록의 제약만 채점되고, None 이면
    기본 여섯 제약 전부를 쓴다::

        from flight_crew.constraints import get_constraints
        fcs.solve(schedule, constraints=get_constraints())
    """
    cfg = _resolve_config(config)
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

    if isinstance(schedule, FlightCrewSchedule):
        _solve(schedule)
        return schedule

    from .models import FlightCrewScheduleModel  # pydantic (선택 의존성)

    model = (
        schedule if isinstance(schedule, FlightCrewScheduleModel) else FlightCrewScheduleModel.model_validate(schedule)
    )
    core = model.to_core()
    _solve(core)
    return FlightCrewScheduleModel.from_core(core)

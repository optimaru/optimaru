"""Food packaging — Timefold engine 기반 C++ 솔버의 Python 래퍼.

Timefold quickstart `food-packaging` 의 포트다: 포장 작업(Job)을 생산 라인
(Line)에 순서대로 배정한다. Java 도메인의 Line.jobs
(@PlanningListVariable, allowsUnassignedValues=true)는 엔진의 LIST 변수로
1:1 매핑되고, "미배정"은 엔진에 추가로 노출되는 unassigned pool 리스트로
표현된다. Java 의 shadow 변수 체인은 리스트 위치에서 파생된다::

    startCleaning(첫 작업)  = line.start          # 청소 구간 길이 0
    startCleaning(그 외)    = 직전 작업의 end
    startProduction         = startCleaning + cleanup(직전 제품 -> 제품)
    end                     = startProduction + duration

Java 의 두 번째 planning variable 인 Line.operator 는 (엔진이 해 하나당
변수 모델 1개만 지원하므로) 솔버 래퍼가 엔진 레그 사이에 힐클라이밍으로
재배정하는 교대 방식으로 최적화된다. operator >= line 이면 단사 배정으로
충돌 제약이 항상 0 이라 한 번의 엔진 실행과 완전히 동치다.

시간은 공통 기준으로부터의 "분"(int64)으로 주고받는다 (데모 인스턴스는
월요일 00:00 == 0분; pydantic 미러 `food_packaging.models` 는 epoch 분과
`datetime.datetime` 을 변환한다).

제약 (Java 는 HardMediumSoftScore; 엔진 HardSoftScore 로의 매핑은
engine.soft = medium * 10^10 + soft, 순위 보존은 |soft| < 10^10 그리고
|medium| < 9*10^5 일 때 정확):
  hard   1. maxEndDateTime           maxEndTime 초과 분만큼 -1
  hard   2. operatorCleaningConflict 같은 operator 라인의 청소 구간
                                     [startCleaning, startProduction) 이
                                     겹치는 작업쌍마다 -겹친 분
  medium 3. idealEndDateTime         idealEndTime 초과 분만큼 -1
  medium 4. maximizeJobsAssigned     미배정 작업마다 -duration 분
  soft   5. minimizeMakespan         라인 마지막 작업마다 -(end - line
                                     start)^2
(minStartTime 과 priority 는 Java 와 동일하게 어떤 제약도 읽지 않는
데이터 필드다.)

사용::

    import food_packaging as fp

    schedule = fp.generate_demo_data()
    fp.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(schedule.score, schedule.score.is_feasible())
"""

from ._core import (
    ConstraintBreakdown,
    Job,
    Line,
    Operator,
    PackagingSchedule,
    Product,
    Score,
    Solver,
    SolverConfig,
    analyze,
    assign_missing_operators,
    calculate_score,
    constraint_costs,
    generate_demo_data,
    improve_operator_assignment,
    list_constraints,
)

__all__ = [
    "ConstraintBreakdown",
    "Job",
    "Line",
    "Operator",
    "PackagingSchedule",
    "Product",
    "Score",
    "Solver",
    "SolverConfig",
    "analyze",
    "assign_missing_operators",
    "calculate_score",
    "generate_demo_data",
    "list_constraints",
    "constraint_costs",
    "improve_operator_assignment",
    "solve",
]


def solve(
    schedule: PackagingSchedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> PackagingSchedule:
    """`PackagingSchedule` 을 in place 로 풀고 그대로 돌려주는 고수준 API.

    ``config`` 가 기본 설정이 된다 (None 이면 엔진 기본값: 30초 예산, ALNS
    켜짐). 명시한 키워드 인자가 config 위에 덮어쓴다.
    ``stop_at_first_feasible`` 은 hard 0 에 도달하는 즉시 종료한다
    (best_score_limit=(0, -1e18)).

    ``constraints`` 로 비용 함수를 직접 고를 수 있다
    (`food_packaging.constraints` 참고). 준 목록의 제약만 채점되고, None
    이면 기본 다섯 제약 전부를 쓴다::

        from food_packaging.constraints import get_constraints
        fp.solve(schedule, constraints=get_constraints())
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

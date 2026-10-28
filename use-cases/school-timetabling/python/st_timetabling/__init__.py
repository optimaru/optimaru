"""School timetabling — Timefold engine 기반 C++ 솔버의 Python 래퍼.

기본 진입점은 canonical 풀-패리티 파이프라인이다: API_SPEC.md 의
`TimetableProblem` JSON 을 `convert_problem` 이 캐노니컬 C++ 도메인
(`_core.canon`, CP-SAT 레퍼런스의 H1~H10 / S1~S6 전 제약)으로 옮기고,
local search 로 푼 뒤 `convert_solution` 이 canonical `Timetable` JSON 으로
되돌린다. `solve` 는 그 전부를 묶은 고수준 진입점이다::

    import st_timetabling as st

    result = st.solve(problem_dict, time_limit_seconds=10.0, random_seed=42)
    print(result["status"], result["penalties"], result["notes"])

quickstart 도메인(교사/학급/교실 충돌 6제약, slotRoom 합성 인코딩)은
`mode="quickstart"` 와 `st_timetabling.quickstart` 로 남아 있다 (근사 변환;
한계는 quickstart 모듈 docstring 참고). `Timeslot`/`Room`/`Lesson`/
`Timetable`/`Solver`/`SolverConfig` 등 최상위 클래스들은 quickstart 도메인,
캐노니컬 도메인 클래스들은 `canon` 서브모듈(`canon.Lesson`, `canon.Timetable`,
`canon.Solver`, `canon.SolverConfig`, `canon.ObjectiveWeights`, ...)에 있다.

비용 함수는 기본이 모놀리식 인크리멘털 계산기(fast path)지만,
`constraints=` 로 플러그인 조립을 골라 쓸 수 있다 (`constraints.py` 참고)::

    from st_timetabling.constraints import Assembly

    result = st.solve(
        problem,
        constraints=Assembly().add("teacher_gap", weight=40).drop("teacher_workday"),
    )

pydantic 미러(직렬화용)는 `st_timetabling.models` 에 있다 (선택 의존성).
"""

import time

from . import quickstart
from ._core import (
    Lesson,
    Room,
    Score,
    Solver,
    SolverConfig,
    Timeslot,
    Timetable,
    calculate_score,
    canon,
)
from .constraints import (
    Assembly,
    AvoidedPeriod,
    BlockAdjacency,
    ClassConflict,
    ClassGap,
    Constraint,
    CourseDailyLimit,
    NonPreferredPeriod,
    RoomCapacity,
    SlotNotAllowed,
    TeacherConflict,
    TeacherConsecutiveLimit,
    TeacherFreeDay,
    TeacherGap,
    TeacherLoadLimit,
    TeacherWorkday,
    UnassignedLesson,
    get_constraints,
    list_constraints,
)
from .convert import (
    ConvertedProblem,
    OccupancyGroup,
    convert_problem,
    convert_solution,
    inject_solution,
)

__all__ = [
    "Assembly",
    "AvoidedPeriod",
    "BlockAdjacency",
    "ClassConflict",
    "ClassGap",
    "Constraint",
    "ConvertedProblem",
    "CourseDailyLimit",
    "Lesson",
    "NonPreferredPeriod",
    "OccupancyGroup",
    "Room",
    "RoomCapacity",
    "Score",
    "SlotNotAllowed",
    "Solver",
    "SolverConfig",
    "TeacherConflict",
    "TeacherConsecutiveLimit",
    "TeacherFreeDay",
    "TeacherGap",
    "TeacherLoadLimit",
    "TeacherWorkday",
    "Timeslot",
    "Timetable",
    "UnassignedLesson",
    "calculate_score",
    "canon",
    "convert_problem",
    "convert_solution",
    "inject_solution",
    "get_constraints",
    "list_constraints",
    "quickstart",
    "solve",
]

# S1~S6 제약 이름 == ObjectiveWeights 필드 이름 (재채점 가중치 접기에 쓴다).
_SOFT_NAMES = (
    "teacher_gap",
    "teacher_workday",
    "teacher_free_day",
    "class_gap",
    "avoided_period",
    "non_preferred_period",
)


def _assemble_plugin(constraints, base_weights):
    """플러그인 경로의 조립: (DecomposedCalculator, 재채점용 ObjectiveWeights,
    미배정 벌점 가중치).

    weight 를 명시하지 않은 S 계열 컴포넌트는 ``base_weights`` (= `weights`
    인자를 얹은 config.weights) 를 따른다 — 모놀리식 경로와 같은 규칙.
    재채점 가중치는 조립을 그대로 반영한다: 오버라이드한 S weight 는 그
    값으로, 조립에서 뺀 S 항은 0 으로. `ObjectiveWeights` 가 정수라 S 계열
    weight 는 정수 값이어야 한다 (아니면 ValueError).

    세 번째 값은 H1(`unassigned_lesson`)을 soft 로 내린 조립에서만 float 이고
    (그 컴포넌트의 유효 weight; 조립에서 아예 뺐으면 0.0), 하드로 남아 있으면
    None 이다 — 미배정이 결과에 남을 수 있는 경우와 그때의 벌점 단가를
    호출자(`convert_solution`)에게 그대로 넘기려는 것이다.
    """
    assembly = constraints if isinstance(constraints, Assembly) else Assembly(constraints)
    calculator = canon.DecomposedCalculator()
    report = dict.fromkeys(_SOFT_NAMES, 0)  # 조립에 없는 S 항은 재채점에서도 0
    unassigned_weight: float | None = 0.0  # 조립에 H1 이 없으면 미배정이 공짜다
    for spec in assembly:
        weight = spec.weight
        if spec.name in report:
            if weight is None:
                weight = getattr(base_weights, spec.name)
            if float(weight) != int(weight) or weight < 0:
                raise ValueError(f"{spec.name} 의 weight 는 음이 아닌 정수여야 한다 (재채점 가중치): {weight!r}")
            report[spec.name] = int(weight)
        if spec.name == UnassignedLesson.name:
            # 하드면 미배정은 애초에 나올 수 없다 (엔진도 무브를 안 켠다).
            unassigned_weight = None if spec.hard is not False else float(1.0 if weight is None else weight)
        calculator.add(spec.name, weight=weight, hard=spec.hard, exponent=spec.exponent)
    return calculator, canon.ObjectiveWeights(**report), unassigned_weight


def _resolve_config(config, *, canonical: bool):
    """config 인자를 모드에 맞는 core SolverConfig 로 변환한다."""
    if config is None:
        return canon.SolverConfig() if canonical else SolverConfig()
    if canonical:
        if isinstance(config, canon.SolverConfig):
            return config
        if hasattr(config, "to_canon"):
            return config.to_canon()
        raise ValueError("canonical 모드의 config 는 canon.SolverConfig 또는 to_canon() 을 가진 객체여야 한다")
    if isinstance(config, SolverConfig):
        return config
    if hasattr(config, "to_core"):
        return config.to_core()
    raise ValueError("quickstart 모드의 config 는 SolverConfig 또는 to_core() 를 가진 객체여야 한다")


def solve(
    problem: dict,
    *,
    mode: str = "canonical",
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    weights: dict | None = None,
    constraints=None,
    on_new_best=None,
    config=None,
) -> dict:
    """canonical `TimetableProblem` dict 를 풀어 canonical `Timetable` dict 로
    돌려주는 고수준 API.

    ``mode="canonical"`` (기본) 은 st::canon 도메인으로 CP-SAT 레퍼런스의
    하드 H1~H10 / 소프트 S1~S6 전부를 그대로 반영한다. ``mode="quickstart"``
    는 quickstart 도메인 근사 변환(미지원 제약은 notes 로 보고)이다.

    ``config`` 가 기본 설정이 된다: 모드에 맞는 `_core` SolverConfig 를
    그대로 주거나, `to_canon()`/`to_core()` 를 가진 객체(`models.SolveConfig`)
    를 줄 수 있다. None 이면 엔진 기본값(30초 예산). 명시한 키워드 인자가
    config 위에 덮어쓴다.

    ``weights`` 는 canonical soft 가중치 dict (teacher_gap / teacher_workday /
    teacher_free_day / class_gap / avoided_period / non_preferred_period 의
    부분집합; 생략한 키는 cpsat 기본값). canonical 모드 전용이다.

    ``constraints`` 는 플러그인 제약 조립 (canonical 모드 전용; `constraints`
    모듈 참고): `Assembly` 또는 스펙/이름의 iterable. ``None`` (기본) 이면
    모놀리식 인크리멘털 계산기의 fast path 를 그대로 쓴다 — 기본 경로의
    동작은 불변이다. 지정하면 조립된 `canon.DecomposedCalculator` 로
    탐색한다: weight 를 명시하지 않은 S 계열 컴포넌트는 ``weights`` 를
    따르고, 탐색은 single-start (parallel_solver_count 무시). 응답의
    penalties 는 항상 canonical 재채점(S1~S6 원시 카운트, 조립과 무관)이며
    objective_value 는 조립을 반영한 유효 S 가중치(오버라이드는 그 값,
    조립에서 뺀 항은 0)로 계산한다 — 그래서 S 계열 weight 는 음이 아닌
    정수여야 한다. status 도 canonical 하드 재채점 기준이다 (예: 하드
    제약을 soft 로 강등한 조립이 그 제약을 위반하면 UNKNOWN).

    H1(`unassigned_lesson`)을 soft 로 내린 조립만은 미배정을 **결과**로
    받아들인다 (cpsat 의 `WeeklyPeriodCounts(hard=false)` = 부분 편성과 같은
    계약): 탐색에 미배정 무브가 붙고, 못 채운 시수는 하드 위반이 아니라
    `penalties["weekly_period_counts"]` 로 보고되며 status 도 그 항을 뺀
    하드 재채점으로 정한다. 미배정 1시간의 값은 그 컴포넌트의 weight 다 —
    다른 소프트 항 합보다 크게 두어야(cpsat 기본 10000) 채울 수 있는 시수를
    전부 채운다.

    ``stop_at_first_feasible`` 은 hard 0 에 도달하는 즉시 종료한다
    (best_score_limit=(0, -1e18)). H1 을 완화한 조립과는 같이 쓰지 않는다 —
    수업을 전부 빼 버린 해도 hard 0 이라 거기서 멈춘다.

    반환 dict 는 API_SPEC.md 의 응답 형태(status/lessons/objective_value/
    best_objective_bound/wall_time_seconds/penalties/notes)를 따른다.
    canonical 모드의 penalties 는 S1~S6 원시 카운트, objective_value 는
    weights 를 적용한 가중 벌점 합 — cpsat 백엔드와 같은 단위다.
    """
    if mode not in ("canonical", "quickstart"):
        raise ValueError(f"알 수 없는 mode: {mode!r} (canonical | quickstart)")
    canonical = mode == "canonical"
    if weights is not None and not canonical:
        raise ValueError("weights 는 canonical 모드에서만 지원된다")
    if constraints is not None and not canonical:
        raise ValueError("constraints 는 canonical 모드에서만 지원된다")

    cfg = _resolve_config(config, canonical=canonical)
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
    if weights is not None:
        cfg.weights = canon.ObjectiveWeights(**weights)

    if canonical:
        converted = convert_problem(problem)
        unassigned_weight = None
        if constraints is None:
            # 기본: 모놀리식 인크리멘털 계산기의 fast path (불변).
            start = time.perf_counter()
            canon.Solver(cfg).solve(converted.table)
            wall = time.perf_counter() - start
        else:
            # 플러그인 경로: 조립된 계산기로 탐색하고, 재채점 가중치
            # (convert_solution 의 objective_value)는 조립을 반영한다.
            calculator, report_weights, unassigned_weight = _assemble_plugin(constraints, cfg.weights)
            start = time.perf_counter()
            canon.Solver(cfg).solve(converted.table, calculator)
            wall = time.perf_counter() - start
            converted.table.weights = report_weights
        return convert_solution(converted, wall_time_seconds=wall, unassigned_weight=unassigned_weight)

    converted = quickstart.convert_problem(problem)
    start = time.perf_counter()
    Solver(cfg).solve(converted.table)
    wall = time.perf_counter() - start
    return quickstart.convert_solution(converted, wall_time_seconds=wall)

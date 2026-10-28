"""패리티 게이트: st::canon 엔진 스코어 == cpsat validate.py (ground truth).

같은 canonical 문제를 (i) CP-SAT(teacher_scheduling)으로 풀어 그 해를
st::canon 도메인에 주입해 엔진 breakdown 을 계산하고, (ii) cpsat 의
validate_timetable(하드 H1~H10) + compute_penalties(소프트 S1~S6)와 대조한다.

게이트 조건 (문제마다):
- 하드 위반 수가 정확히 일치: cpsat 해는 validate 상 위반 0 이어야 하고,
  엔진 hard_total() 도 0 이어야 한다.
- 소프트 원시 카운트(S1~S6)가 key 별로 정확히 일치.
- 가중 벌점 합이 정확히 일치: 엔진 soft_penalty(weights) == validate 가중 합
  == cpsat 이 보고한 objective_value.

반대 방향(엔진 solve -> cpsat validate)도 검증해 변환/역변환까지 포함한
파이프라인 전체의 의미론이 같음을 고정한다.
"""

import pytest

pytest.importorskip("teacher_scheduling")

from teacher_scheduling import SolverConfig as CpsatConfig
from teacher_scheduling import Timetable, TimetableProblem
from teacher_scheduling import solve as solve_cpsat
from teacher_scheduling.sample import build_sample_problem
from teacher_scheduling.validate import compute_penalties, validate_timetable

import st_timetabling as st
from st_timetabling import canon
from st_timetabling.constraints import Assembly

# 문제 3: 분반(선호/기피 마스크가 서로 다른 멤버) + 연강 + 금지 시간 +
# 학급 불가 시간 + 교사 연속 상한/희망 공강일까지 켠 소형 인스턴스.
_FEATURE_WEIGHTS = {
    "teacher_gap": 7,
    "teacher_workday": 2,
    "teacher_free_day": 11,
    "class_gap": 3,
    "avoided_period": 5,
    "non_preferred_period": 2,
}


def feature_problem() -> dict:
    return {
        "calendar": {"periods_per_day": {"0": 5, "1": 5, "2": 5, "3": 5, "4": 5}},
        "subjects": [
            {"id": "MATH", "name": "수학", "preferred_periods": [1, 2]},
            {"id": "KOR", "name": "국어"},
            {"id": "SCI", "name": "과학", "required_room_type": "과학실", "avoided_periods": [5]},
            {"id": "TECH", "name": "기술", "required_room_type": "기술실", "avoided_periods": [4]},
            {"id": "HOME", "name": "가정", "preferred_periods": [3, 4]},
            {"id": "ART", "name": "미술", "avoided_periods": [1]},
            {"id": "HR", "name": "창체"},
        ],
        "teachers": [
            {"id": "T1", "name": "김수학", "max_consecutive_periods": 2},
            {"id": "T2", "name": "이국어", "unavailable": [{"day": 0, "period": 1}], "preferred_free_days": [4]},
            {"id": "T3", "name": "박과학"},
            {"id": "T4", "name": "권기술"},
            {"id": "T5", "name": "송가정"},
            {"id": "T6", "name": "구미술"},
            {"id": "T7", "name": "담임"},
        ],
        "class_groups": [{"id": "C1", "name": "1학년 1반", "grade": 1, "unavailable": [{"day": 4, "period": 5}]}],
        "rooms": [
            {"id": "R_SCI", "name": "과학실", "room_type": "과학실"},
            {"id": "R_TECH", "name": "기술실", "room_type": "기술실"},
        ],
        "courses": [
            {
                "id": "C1-MATH",
                "class_group_id": "C1",
                "subject_id": "MATH",
                "teacher_ids": ["T1"],
                "periods_per_week": 4,
            },
            {
                "id": "C1-KOR",
                "class_group_id": "C1",
                "subject_id": "KOR",
                "teacher_ids": ["T2"],
                "periods_per_week": 3,
                "forbidden_slots": [{"day": 1, "period": 1}],
            },
            # 팀티칭: 두 교사가 함께 들어간다.
            {
                "id": "C1-SCI",
                "class_group_id": "C1",
                "subject_id": "SCI",
                "teacher_ids": ["T3", "T7"],
                "periods_per_week": 3,
            },
            # 분반: 기피/선호 마스크가 서로 다른 두 강좌가 같은 시간에 진행된다.
            {
                "id": "C1-TECH",
                "class_group_id": "C1",
                "subject_id": "TECH",
                "teacher_ids": ["T4"],
                "periods_per_week": 2,
                "parallel_group": "C1-분반",
            },
            {
                "id": "C1-HOME",
                "class_group_id": "C1",
                "subject_id": "HOME",
                "teacher_ids": ["T5"],
                "periods_per_week": 2,
                "parallel_group": "C1-분반",
            },
            # 주 1회 2연강.
            {
                "id": "C1-ART",
                "class_group_id": "C1",
                "subject_id": "ART",
                "teacher_ids": ["T6"],
                "periods_per_week": 2,
                "block_periods": 2,
            },
            {
                "id": "C1-HR",
                "class_group_id": "C1",
                "subject_id": "HR",
                "teacher_ids": ["T7"],
                "periods_per_week": 1,
                "fixed_slots": [{"day": 0, "period": 1}],
            },
        ],
    }


def _weighted(penalties: dict[str, int], weights: dict[str, int]) -> int:
    return sum(weights[name] * count for name, count in penalties.items())


def _assert_parity(problem_dict: dict, cpsat_result: Timetable, weights: dict[str, int]) -> None:
    """cpsat 해를 엔진에 주입해 하드/소프트가 validate.py 와 일치하는지 본다."""
    problem = TimetableProblem.model_validate(problem_dict)

    # ground truth: cpsat validate 의 하드 위반 목록 + 소프트 원시 카운트.
    violations = validate_timetable(problem, cpsat_result)
    truth_penalties = compute_penalties(problem, cpsat_result)

    # 엔진: 같은 해를 st::canon 에 주입해 breakdown 을 recompute.
    converted = st.convert_problem(problem_dict)
    converted.table.weights = canon.ObjectiveWeights(**weights)
    st.inject_solution(converted, [lesson.model_dump(mode="json") for lesson in cpsat_result.lessons])
    breakdown = canon.compute_breakdown(converted.table)

    # 하드 위반 수가 정확히 일치 (유효한 cpsat 해 => 양쪽 다 0).
    assert violations == []
    assert breakdown.hard_total() == 0

    # 소프트: 원시 카운트 key 별 일치 + 가중 합 일치 (cpsat 보고값 포함).
    assert dict(breakdown.penalties()) == dict(truth_penalties)
    engine_soft = breakdown.soft_penalty(canon.ObjectiveWeights(**weights))
    assert engine_soft == _weighted(truth_penalties, weights)
    assert engine_soft == cpsat_result.objective_value

    # score 경로도 같은 값을 내야 한다 (calculate_score == breakdown).
    score = canon.calculate_score(converted.table)
    assert score.hard == 0
    assert -score.soft == engine_soft


def _solve_and_assert(problem_dict: dict, *, weights: dict[str, int] | None = None, time_limit: float = 10.0) -> None:
    weights = weights or CpsatConfig().weights.model_dump()
    config = CpsatConfig(time_limit_seconds=time_limit, num_workers=8, random_seed=7, weights=weights)
    result = solve_cpsat(TimetableProblem.model_validate(problem_dict), config)
    assert result.status.has_solution
    _assert_parity(problem_dict, result, weights)


def test_parity_sample_problem():
    """cpsat 데모 학교 (34칸 완전 밀집, 연강/분반/팀티칭/고정/상한 전부)."""
    _solve_and_assert(build_sample_problem().model_dump(mode="json"))


def test_parity_demo_fixture(problem):
    """python 데모 문제 (20칸 완전 밀집, 팀티칭/특별실/고정/불가 시간)."""
    _solve_and_assert(problem, time_limit=10.0)


def test_parity_feature_problem_with_custom_weights():
    """소형 종합 인스턴스 + 비기본 가중치 — 가중 합 패리티까지 확인."""
    _solve_and_assert(feature_problem(), weights=_FEATURE_WEIGHTS, time_limit=10.0)


def test_parity_partial_solution(problem):
    """부분 편성(H1 완화)도 같은 계약이다 — cpsat solve_partial 이 ground truth.

    미배정 수를 맞출 수는 없다(local search 는 최적성을 증명하지 못한다). 고정
    하는 것은 **의미론**이다: 미배정은 하드 위반이 아니고(FEASIBLE), 배정한
    부분은 하드 제약을 전부 지키며, 미배정 시수는 cpsat 과 같은 이름의 벌점
    항으로 보고되고 그 수만큼 cpsat 의 최적값보다 나쁠 수는 있어도 좋을 수는
    없다.
    """
    from teacher_scheduling.partial import solve_partial

    problem["calendar"]["periods_per_day"]["4"] = 0  # 금요일을 지운다: 20시수 / 16칸
    required = sum(course["periods_per_week"] for course in problem["courses"])

    # ground truth: cpsat 완화 모드의 최적 부분해.
    optimum = solve_partial(
        TimetableProblem.model_validate(problem),
        CpsatConfig(time_limit_seconds=10.0, num_workers=8, random_seed=7),
    )
    assert optimum.status.has_solution

    result = st.solve(
        problem,
        constraints=Assembly().add("weekly_period_counts", hard=False, weight=10_000),
        time_limit_seconds=10.0,
        random_seed=7,
    )

    # 미배정은 탐색 실패가 아니라 결과다.
    assert result["status"] == "FEASIBLE"
    assert result["penalties"]["weekly_period_counts"] == required - len(result["lessons"])
    # 최적해보다 더 채울 수는 없다 (cpsat 은 최적성을 증명한다).
    assert 0 < len(result["lessons"]) <= len(optimum.lessons)

    # 배정한 부분은 하드 제약을 전부 지킨다 — 주당 시수를 실제 배정된 수로
    # 줄인 문제로 보면 cpsat validate 가 위반을 하나도 못 찾아야 한다
    # (H1 만 빼고 그대로 검사하는 것과 같다).
    timetable = Timetable.model_validate(result)
    scheduled: dict[str, int] = {}
    for lesson in timetable.lessons:
        scheduled[lesson.course_id] = scheduled.get(lesson.course_id, 0) + 1
    shrunk = {
        **problem,
        "courses": [c | {"periods_per_week": n} for c in problem["courses"] if (n := scheduled.get(c["id"], 0))],
    }
    assert validate_timetable(TimetableProblem.model_validate(shrunk), timetable) == []


def test_parity_engine_solution_backwards():
    """반대 방향: 엔진 canonical solve 결과를 cpsat validate 로 재검증."""
    problem_dict = build_sample_problem().model_dump(mode="json")
    result = st.solve(
        problem_dict,
        time_limit_seconds=15.0,
        random_seed=42,
        parallel_solver_count=4,
        stop_at_first_feasible=True,  # 패리티는 해 품질과 무관하다 — 첫 hard 0 이면 충분
    )
    assert result["status"] == "FEASIBLE"
    assert result["notes"] == []

    problem = TimetableProblem.model_validate(problem_dict)
    timetable = Timetable.model_validate(result)
    assert validate_timetable(problem, timetable) == []

    truth_penalties = compute_penalties(problem, timetable)
    assert dict(result["penalties"]) == dict(truth_penalties)
    default_weights = CpsatConfig().weights.model_dump()
    assert result["objective_value"] == _weighted(truth_penalties, default_weights)

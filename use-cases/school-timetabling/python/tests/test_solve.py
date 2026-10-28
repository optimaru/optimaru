"""고수준 solve() 검증: canonical(기본)/quickstart 모드와 config/weights 인자."""

import pytest

import st_timetabling as st
from st_timetabling.models import SolveConfig


def test_demo_problem_solves_feasible(problem):
    result = st.solve(
        problem,
        time_limit_seconds=20.0,
        random_seed=42,
        stop_at_first_feasible=True,
    )

    assert result["status"] == "FEASIBLE"
    assert result["wall_time_seconds"] > 0.0
    assert result["best_objective_bound"] is None
    assert len(result["lessons"]) == 40
    # canonical 모드는 풀-패리티라 미지원 notes 가 없다.
    assert result["notes"] == []

    # H3/H4: 학급/교사(팀티칭 부교사 포함)는 같은 시간에 수업 하나만 갖는다.
    class_slots = set()
    teacher_slots = set()
    for lesson in result["lessons"]:
        slot = (lesson["slot"]["day"], lesson["slot"]["period"])
        assert (lesson["class_group_id"], slot) not in class_slots
        class_slots.add((lesson["class_group_id"], slot))
        for teacher_id in lesson["teacher_ids"]:
            assert (teacher_id, slot) not in teacher_slots
            teacher_slots.add((teacher_id, slot))

    # H5 확장: 김국어(T_KOR)는 월요일 1교시 불가.
    for lesson in result["lessons"]:
        if "T_KOR" in lesson["teacher_ids"]:
            assert (lesson["slot"]["day"], lesson["slot"]["period"]) != (0, 1)

    # H1: 강좌별 시수가 보존된다.
    per_course: dict[str, int] = {}
    for lesson in result["lessons"]:
        per_course[lesson["course_id"]] = per_course.get(lesson["course_id"], 0) + 1
    for course in problem["courses"]:
        assert per_course[course["id"]] == course["periods_per_week"]

    # H6: 특별실 수업은 전부 교실을 받는다 (hard 0 이면 사후 그리디가 항상 성공).
    for lesson in result["lessons"]:
        if lesson["subject_id"] == "SCI":
            assert lesson["room_id"] == "R_SCI"
        elif lesson["subject_id"] == "PE":
            assert lesson["room_id"] == "R_GYM"
        else:
            assert lesson["room_id"] is None

    # penalties 는 S1~S6 원시 카운트, objective_value 는 기본 가중치 가중 합.
    assert set(result["penalties"]) == {
        "teacher_gap",
        "teacher_workday",
        "teacher_free_day",
        "class_gap",
        "avoided_period",
        "non_preferred_period",
    }
    default_weights = {
        "teacher_gap": 20,
        "teacher_workday": 3,
        "teacher_free_day": 30,
        "class_gap": 10,
        "avoided_period": 5,
        "non_preferred_period": 1,
    }
    assert result["objective_value"] == sum(
        default_weights[name] * count for name, count in result["penalties"].items()
    )


def test_solve_quickstart_mode(problem):
    result = st.solve(
        problem,
        mode="quickstart",
        time_limit_seconds=10.0,
        random_seed=42,
        stop_at_first_feasible=True,
    )
    assert result["status"] in ("FEASIBLE", "UNKNOWN")
    assert len(result["lessons"]) == 40
    # quickstart 근사 변환의 한계는 notes 로 남는다 (penalties 는 비어 있다).
    assert any("soft" in note for note in result["notes"])
    assert result["penalties"] == {}


def test_solve_accepts_config_objects(problem):
    # models.SolveConfig 는 두 모드 모두에서 config 로 쓸 수 있다.
    model_cfg = SolveConfig(late_acceptance_size=100)
    result = st.solve(
        problem,
        time_limit_seconds=10.0,
        random_seed=1,
        stop_at_first_feasible=True,
        config=model_cfg,
    )
    assert result["status"] in ("FEASIBLE", "UNKNOWN")
    assert len(result["lessons"]) == 40

    # 모드별 _core config 를 직접 줄 수도 있다.
    cfg = st.canon.SolverConfig()
    cfg.late_acceptance_size = 100
    result = st.solve(
        problem,
        time_limit_seconds=5.0,
        random_seed=1,
        stop_at_first_feasible=True,
        config=cfg,
    )
    assert result["status"] in ("FEASIBLE", "UNKNOWN")

    # 모드와 config 타입이 어긋나면 명확한 ValueError.
    with pytest.raises(ValueError, match="config"):
        st.solve(problem, config=st.SolverConfig())
    with pytest.raises(ValueError, match="config"):
        st.solve(problem, mode="quickstart", config=st.canon.SolverConfig())


def test_solve_weights_override(problem):
    weights = {"teacher_gap": 0, "teacher_workday": 1, "class_gap": 2}
    result = st.solve(
        problem,
        time_limit_seconds=5.0,
        random_seed=3,
        stop_at_first_feasible=True,
        weights=weights,
    )
    # 명시하지 않은 키는 cpsat 기본값을 유지한다.
    effective = {
        "teacher_gap": 0,
        "teacher_workday": 1,
        "teacher_free_day": 30,
        "class_gap": 2,
        "avoided_period": 5,
        "non_preferred_period": 1,
    }
    assert result["objective_value"] == sum(effective[name] * count for name, count in result["penalties"].items())

    with pytest.raises(ValueError, match="canonical"):
        st.solve(problem, mode="quickstart", weights=weights)
    with pytest.raises(ValueError, match="mode"):
        st.solve(problem, mode="cpsat")

"""플러그인 제약 조립 검증: 조립 API + `solve(constraints=...)` 경로.

모놀리식 fast path(기본, constraints=None)는 불변이고, 플러그인 경로는
(1) 기본 조립이 모놀리식과 같은 점수를 내고 solve 결과가 canonical 재채점과
일치하는지, (2) weight 오버라이드가 탐색 점수와 결과 objective_value 에
반영되는지, (3) 컴포넌트 제외가 동작하는지, (4) 잘못된 이름이 ValueError 인지
를 고정한다.
"""

import random

import pytest

import st_timetabling as st
from st_timetabling import canon
from st_timetabling.constraints import Assembly, TeacherGap, list_constraints

# cpsat ObjectiveWeights 기본값 (test_solve.py 와 동일한 기준).
DEFAULT_WEIGHTS = {
    "teacher_gap": 20,
    "teacher_workday": 3,
    "teacher_free_day": 30,
    "class_gap": 10,
    "avoided_period": 5,
    "non_preferred_period": 1,
}


def _tiny_table() -> "canon.Timetable":
    """손으로 채점 가능한 최소 인스턴스: 1일 4교시, 교사/학급 1, 수업 2개.

    1교시/3교시에 배정하면 S1 teacher_gap = 1, S2 teacher_workday = 1,
    S4 class_gap = 1 이고 나머지는 전부 0 이다.
    """
    lessons = [canon.Lesson(f"L{k}", 0, k, [0]) for k in range(2)]
    table = canon.Timetable([0, 0, 0, 0], [1, 2, 3, 4], lessons)
    table.teacher_count = 1
    table.class_count = 1
    table.teacher_max_per_day = [6]
    table.teacher_max_per_week = [-1]
    table.teacher_max_consecutive = [4]
    table.teacher_preferred_free_days = [0]
    table.course_daily_limit = [1, 1]
    table.set_assignment([0, 2])  # 1교시, 3교시 -> 사이에 공강 1
    return table


def _weighted(penalties: dict[str, int], weights: dict[str, int]) -> float:
    return float(sum(weights[name] * count for name, count in penalties.items()))


def test_list_constraints_matches_core_registry():
    names = list_constraints()
    assert names == tuple(canon.CONSTRAINT_NAMES)
    assert len(names) == 15
    # 기본 Assembly 는 레지스트리 전부를 같은 순서로 담는다.
    assert tuple(spec.name for spec in Assembly()) == names


def test_default_assembly_matches_monolithic_score(problem):
    """(1) 기본 조립 == 모놀리식: 무작위 배정들에서 점수가 비트 단위로 같다."""
    converted = st.convert_problem(problem)
    canon.prepare(converted.table)
    calculator = canon.default_calculator()  # 전체 15개, cpsat 기본 가중치
    rng = random.Random(42)
    slot_count = converted.table.slot_count()
    lesson_count = len(converted.table.lessons)
    for _ in range(30):
        converted.table.set_assignment([rng.randrange(-1, slot_count) for _ in range(lesson_count)])
        mono = canon.calculate_score(converted.table)
        plugin = calculator.score(converted.table)
        assert (plugin.hard, plugin.soft) == (mono.hard, mono.soft)


def test_plugin_solve_default_assembly_parity(problem):
    """(1) 기본 조립 solve: 하드 0 + canonical 재채점(모놀리식 breakdown)과
    penalties/objective_value 가 일치한다."""
    result = st.solve(
        problem,
        constraints=Assembly(),
        time_limit_seconds=20.0,
        random_seed=7,
        stop_at_first_feasible=True,
    )
    assert result["status"] == "FEASIBLE"
    assert result["notes"] == []
    assert len(result["lessons"]) == 40

    # 결과 해를 새 변환에 주입해 모놀리식 경로로 재채점 — 완전 일치해야 한다.
    converted = st.convert_problem(problem)
    st.inject_solution(converted, result["lessons"])
    breakdown = canon.compute_breakdown(converted.table)
    assert breakdown.hard_total() == 0
    assert dict(breakdown.penalties()) == result["penalties"]
    assert result["objective_value"] == _weighted(result["penalties"], DEFAULT_WEIGHTS)


def test_weight_override_applies_to_score_and_result(problem):
    """(2) weight 오버라이드: 계산기 점수와 solve 결과 양쪽에 반영된다."""
    # 손채점 인스턴스: teacher_gap 1 이라 가중치가 그대로 비용이 된다.
    table = _tiny_table()
    default_soft = canon.default_calculator().score(table).soft

    doubled = canon.DecomposedCalculator(Assembly().add("teacher_gap", weight=40))
    score = doubled.score(table)
    assert score.hard == 0
    assert score.soft == default_soft - 20  # gap 1 x (40 - 20)
    totals = {t["name"]: t["cost"] for t in doubled.constraint_totals()}
    assert totals["S1 teacher gap"] == 40.0

    # 스펙 인스턴스 리스트(es 스타일)도 같은 조립이 된다.
    spec_list = canon.DecomposedCalculator([TeacherGap(weight=40)])
    assert spec_list.score(table).soft == -40.0

    # solve 결과: objective_value 가 오버라이드된 유효 가중치로 재채점된다.
    result = st.solve(
        problem,
        constraints=Assembly().add("teacher_gap", weight=40),
        time_limit_seconds=20.0,
        random_seed=7,
        stop_at_first_feasible=True,
    )
    assert result["status"] == "FEASIBLE"
    effective = dict(DEFAULT_WEIGHTS, teacher_gap=40)
    assert result["objective_value"] == _weighted(result["penalties"], effective)


def test_drop_component(problem):
    """(3) 컴포넌트 제외: 계산기에서 항이 사라지고 재채점 가중치도 0 이 된다."""
    assembly = Assembly().drop("teacher_workday")
    assert len(assembly) == 14
    assert "teacher_workday" not in assembly
    assert "teacher_gap" in assembly

    table = _tiny_table()
    calculator = canon.DecomposedCalculator(assembly)
    score = calculator.score(table)
    names = {t["name"] for t in calculator.constraint_totals()}
    assert "S2 teacher workday" not in names
    # 전체 조립 대비 정확히 workday 1 x 3 만큼 벌점이 빠진다.
    assert score.soft == canon.default_calculator().score(table).soft + 3

    result = st.solve(
        problem,
        constraints=assembly,
        time_limit_seconds=20.0,
        random_seed=7,
        stop_at_first_feasible=True,
    )
    assert result["status"] == "FEASIBLE"
    effective = dict(DEFAULT_WEIGHTS, teacher_workday=0)
    assert result["objective_value"] == _weighted(result["penalties"], effective)


# ------------------------------------------------------------------ 부분 편성 (H1 완화)
#
# H1 을 soft 로 내리면 "배정하지 않는다" 가 탐색의 선택지가 된다 (미배정 무브).
# cpsat 의 WeeklyPeriodCounts(hard=false) = solve_partial 과 같은 계약이다.


def _over_constrained(problem: dict) -> dict:
    """금요일을 지운 데모 학교 — 학급마다 주 20시수인데 칸이 16개뿐이다."""
    problem["calendar"]["periods_per_day"]["4"] = 0
    return problem


def test_relaxed_h1_returns_a_partial_timetable(problem):
    """못 채운 시수는 하드 위반이 아니라 미배정으로 남는다."""
    problem = _over_constrained(problem)
    relaxed = Assembly().add("weekly_period_counts", hard=False, weight=10_000)

    strict = st.solve(problem, time_limit_seconds=5.0, random_seed=7)
    partial = st.solve(problem, constraints=relaxed, time_limit_seconds=5.0, random_seed=7)

    # 하드로 두면 넣을 수 없는 시수가 하드 위반으로 남아 증명 없이 UNKNOWN 이다.
    assert strict["status"] == "UNKNOWN"
    assert len(strict["lessons"]) == 40, "H1 이 하드면 탐색은 모든 수업에 슬롯을 준다"

    # 완화하면 나머지는 배정한 부분해다: status 는 미배정을 빼고 정해진다.
    assert partial["status"] == "FEASIBLE"
    assert 0 < len(partial["lessons"]) < 40
    # 미배정 시수는 cpsat 완화 모드와 같은 이름·같은 단위로 보고된다.
    assert partial["penalties"]["weekly_period_counts"] == 40 - len(partial["lessons"])
    assert partial["objective_value"] == _weighted(
        partial["penalties"], dict(DEFAULT_WEIGHTS, weekly_period_counts=10_000)
    )
    # 칸이 32개(2학급 x 16)뿐이라 그 이상은 애초에 불가능하다.
    assert len(partial["lessons"]) <= 32


def test_relaxed_h1_does_not_unassign_what_fits(problem):
    """풀리는 문제에서는 완화해도 한 칸도 빼지 않는다 (벌점이 그만큼 비싸다).

    `stop_at_first_feasible` 은 쓰지 않는다 — H1 을 완화하면 "다 빼 버린 해"
    도 hard 0 이라 첫 feasible 에서 멈추면 그게 답이 되어 버린다.
    """
    result = st.solve(
        problem,
        constraints=Assembly().add("weekly_period_counts", hard=False, weight=10_000),
        time_limit_seconds=10.0,
        random_seed=7,
    )
    assert result["status"] == "FEASIBLE"
    assert len(result["lessons"]) == 40
    assert result["penalties"]["weekly_period_counts"] == 0
    assert result["notes"] == []


def test_h1_stays_hard_without_the_override(problem):
    """조립을 줘도 H1 을 안 건드렸으면 미배정은 여전히 탐색 실패다."""
    problem = _over_constrained(problem)
    result = st.solve(
        problem,
        constraints=Assembly().add("teacher_gap", weight=40),
        time_limit_seconds=5.0,
        random_seed=7,
    )
    assert result["status"] == "UNKNOWN"
    assert len(result["lessons"]) == 40
    assert "weekly_period_counts" not in result["penalties"]


def test_unknown_or_invalid_constraint_raises(problem):
    """(4) 잘못된 이름/사용은 ValueError 로 즉시 실패한다."""
    with pytest.raises(ValueError, match="알 수 없는 제약"):
        Assembly().add("teacher_gaps")
    with pytest.raises(ValueError, match="뺄 수 없다"):
        Assembly().drop("no_such_constraint")
    with pytest.raises(ValueError, match="unknown constraint"):
        canon.DecomposedCalculator().add("no_such_constraint")
    # 스펙 인스턴스 + 키워드 오버라이드 혼용 금지.
    with pytest.raises(ValueError, match="스펙 인스턴스"):
        Assembly().add(TeacherGap(), weight=40)
    # constraints 는 canonical 모드 전용.
    with pytest.raises(ValueError, match="canonical"):
        st.solve(problem, mode="quickstart", constraints=Assembly())
    # 재채점 가중치(ObjectiveWeights)는 정수라 S 계열 weight 는 정수만 허용.
    with pytest.raises(ValueError, match="정수"):
        st.solve(problem, constraints=Assembly().add("teacher_gap", weight=2.5), time_limit_seconds=1.0)


# ---------------------------------------------------------------- cpsat 이름 별칭


def test_cpsat_names_resolve_to_engine_names():
    """같은 조립 선언을 두 백엔드에 그대로 보낼 수 있어야 한다 (성능 비교의 전제)."""
    from st_timetabling.constraints import CPSAT_ALIASES, resolve_name

    assert resolve_name("weekly_period_counts") == "unassigned_lesson"
    assert resolve_name("teacher_conflicts") == "teacher_conflict"
    # 이름이 이미 같은 것과 엔진 이름은 그대로 통과한다.
    assert resolve_name("room_capacity") == "room_capacity"
    assert resolve_name("teacher_gap") == "teacher_gap"
    assert resolve_name("unassigned_lesson") == "unassigned_lesson"

    assert set(CPSAT_ALIASES.values()) <= set(list_constraints())


def test_alias_map_covers_every_cpsat_constraint():
    """cpsat 카탈로그의 모든 이름이 엔진 스펙이거나 구조적으로 보장되는 것이어야 한다.

    cpsat 패키지가 있을 때만 돈다 — 두 패키지가 갈라지면 여기서 잡힌다.
    """
    catalog = pytest.importorskip("teacher_scheduling.api").catalog
    from st_timetabling.constraints import STRUCTURAL_IN_ENGINE, resolve_name

    engine_names = set(list_constraints())
    for spec in catalog():
        if spec.name in STRUCTURAL_IN_ENGINE:
            continue
        assert resolve_name(spec.name) in engine_names, f"엔진에 대응이 없다: {spec.name}"


def test_structural_constraints_are_rejected_with_a_reason():
    """분반/고정배치는 엔진에서 조립 대상이 아니다 — 조용히 무시하지 않는다."""
    with pytest.raises(ValueError, match="변환 단계가 보장"):
        Assembly().add("parallel_groups")
    with pytest.raises(ValueError, match="변환 단계가 보장"):
        Assembly().add("fixed_slots")


def test_assembly_accepts_cpsat_names_end_to_end(problem):
    """cpsat 이름으로 조립해도 엔진 이름으로 조립한 것과 같은 결과."""
    by_cpsat_name = Assembly().add("teacher_gap", weight=40).drop("teacher_workday")
    by_engine_name = Assembly().add("teacher_gap", weight=40).drop("teacher_workday")
    assert repr(by_cpsat_name) == repr(by_engine_name)

    # 별칭이 필요한 이름으로도 같은 자리를 교체한다 (중복 추가가 아니다).
    assembly = Assembly().add("weekly_period_counts", hard=False, weight=10_000)
    assert len(assembly) == len(Assembly())
    assert "unassigned_lesson" in assembly
    assert "weekly_period_counts" in assembly, "__contains__ 도 별칭을 받는다"

    result = st.solve(problem, constraints=assembly, time_limit_seconds=5.0, random_seed=7)
    assert result["status"] in ("FEASIBLE", "UNKNOWN")

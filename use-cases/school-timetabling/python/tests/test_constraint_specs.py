"""st_timetabling.constraints.get_constraints — 기본 비용 함수를 한 줄로.

기존 `Assembly` 체이닝 빌더와 스펙 클래스는 그대로다. 여기서 보는 건 레포의
다른 열다섯 use case 와 같은 진입점인 `get_constraints()` 가 기본 열다섯
제약을 정확히 돌려주는지, 그리고 그게 solve 경로에 그대로 먹히는지다.
"""

import pytest

import st_timetabling as st
from st_timetabling import canon
from st_timetabling.constraints import (
    Assembly,
    TeacherGap,
    get_constraints,
    list_constraints,
)


def test_get_constraints_defaults_to_the_full_registry():
    """기본 목록은 코어 레지스트리와 이름도 순서도 같다."""
    assert tuple(spec.name for spec in get_constraints()) == list_constraints()
    assert len(get_constraints()) == 15


def test_get_constraints_matches_the_default_assembly():
    """인자 없는 `Assembly()` 와 같은 세트여야 한다 — 진입점만 다르다."""
    assert [spec.name for spec in get_constraints()] == [spec.name for spec in Assembly()]


def test_passing_a_list_uses_it_as_given():
    """준 목록을 그대로 쓴다 — 목록에 있는 제약만 채점된다."""
    picked = get_constraints([TeacherGap(weight=40)])
    assert [spec.name for spec in picked] == ["teacher_gap"]
    assert picked[0].weight == 40


def test_empty_list_is_rejected_not_treated_as_the_defaults():
    """빈 목록은 기본값이 **아니다** — 필터 결과가 우연히 비면 에러여야 한다.

    "아무것도 안 고름" 이 조용히 "전부 채점" 으로 뒤집히면 안 되고, C++
    조립 경로도 같은 계약(빈 목록은 value_error)을 쓴다."""
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])


def test_returned_list_is_a_fresh_copy():
    """돌려준 목록을 호출자가 고쳐도 다음 호출에 영향이 없어야 한다."""
    first = get_constraints()
    first.pop()
    assert len(get_constraints()) == 15


def test_specs_carry_the_documented_defaults():
    """스펙 기본값은 weight None / hard None / exponent 1.0 이다."""
    spec = get_constraints()[0]
    assert spec.weight is None
    assert spec.hard is None
    assert spec.exponent == pytest.approx(1.0)


def test_default_assembly_matches_monolithic_score(problem):
    """기본 목록으로 조립한 계산기는 모놀리식 채점과 같은 값을 내야 한다."""
    converted = st.convert_problem(problem)
    canon.prepare(converted.table)
    calculator = canon.DecomposedCalculator(get_constraints())
    mono = canon.calculate_score(converted.table)
    plugin = calculator.score(converted.table)
    assert (plugin.hard, plugin.soft) == (mono.hard, mono.soft)


def test_solve_accepts_the_spec_list(problem):
    """`solve(constraints=get_constraints())` 가 Assembly 를 준 것과 같이 동작한다."""
    result = st.solve(
        problem,
        constraints=get_constraints(),
        time_limit_seconds=10.0,
        random_seed=7,
        stop_at_first_feasible=True,
    )
    assert result["status"] == "FEASIBLE"
    assert result["notes"] == []
    assert len(result["lessons"]) == 40
    assert result["objective_value"] is not None

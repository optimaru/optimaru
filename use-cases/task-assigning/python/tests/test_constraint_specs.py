"""task_assigning.constraints — 비용 함수를 선언으로 고르는 API.

제약 하나하나의 채점값 스팟체크는 `test_constraints.py` 가 맡는다. 여기서는
"어떤 제약을 켤지" 를 고르는 선택 경로만 본다.
"""

import pytest

import task_assigning as ta
from task_assigning.constraints import (
    Constraint,
    CriticalPriorityTaskEndTime,
    MinimizeMakespan,
    critical_priority_task_end_time,
    get_constraints,
    minimize_makespan,
    minimize_unassigned_tasks,
    no_missing_skills,
    score,
)
from task_assigning.demo import generate_demo_data


def build_solution():
    return generate_demo_data().to_core()


def solve(constraints=None):
    solution = build_solution()
    ta.solve(solution, time_limit_seconds=0.6, random_seed=1, constraints=constraints)
    return solution.score


def test_list_constraints_matches_the_spec_names():
    """스펙이 들고 있는 이름은 코어가 실제로 아는 이름이어야 한다."""
    # unit 제약이 먼저, global 제약(minimize_unassigned_tasks)이 뒤.
    available = ta.list_constraints()
    assert available == [
        "no_missing_skills",
        "minimize_makespan",
        "critical_priority_task_end_time",
        "major_priority_task_end_time",
        "minor_priority_task_end_time",
        "minimize_unassigned_tasks",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == ta.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """기본 스펙 전부로 조립한 비용 함수는 모놀리식 채점과 같은 값을 내야 한다."""
    solution = build_solution()
    ta.solve(
        solution,
        time_limit_seconds=0.6,
        random_seed=1,
        constraints=get_constraints(),
    )
    recomputed = ta.calculate_score(solution)
    assert solution.score.hard == recomputed.hard
    assert solution.score.soft0 == recomputed.soft0
    assert solution.score.soft1 == pytest.approx(recomputed.soft1)
    assert solution.score.soft2 == pytest.approx(recomputed.soft2)


def test_picking_a_subset_drops_the_other_terms():
    """목록에 없는 제약은 채점되지 않는다 — 그 레벨이 통째로 비워진다.

    Bendable 네 레벨이다: hard = 스킬 부족, soft0 = 미배정,
    soft1 = 메이크스팬, soft2 = 우선순위 종료시각."""
    baseline = solve()
    assert baseline.soft1 < 0 and baseline.soft2 < 0

    hard_only = solve([no_missing_skills, minimize_unassigned_tasks])
    assert hard_only.soft1 == 0
    assert hard_only.soft2 == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """메이크스팬 가중치만 올리면 소프트만 커지고 하드는 그대로."""
    picks = [no_missing_skills, minimize_unassigned_tasks]
    base = solve([*picks, minimize_makespan])
    heavier = solve([*picks, MinimizeMakespan(weight=10**9)])
    assert heavier.hard == base.hard
    assert heavier.soft1 < base.soft1  # 벌점이므로 더 음수


def test_level_can_be_flipped():
    """소프트 제약을 하드로 올리면 그 벌점이 하드 쪽으로 옮겨간다.

    태스크를 다 배정하는 이상 우선순위 종료시각은 0 이 될 수 없다. 레벨을
    올리면 그 비용이 엔진의 접힌 하드 레벨로 들어가고, 되돌려 읽을 때
    kMissingSkillUnit(10^9) 아래라 soft0 자리에 나타난다 — 벌점이 soft2
    에서 soft0 으로 통째로 이동한다."""
    picks = [minimize_unassigned_tasks]

    soft_level = solve([*picks, critical_priority_task_end_time])
    assert soft_level.soft2 < 0
    assert soft_level.soft0 == 0

    hard_level = solve([*picks, CriticalPriorityTaskEndTime(hard=True)])
    assert hard_level.soft2 == 0
    assert hard_level.soft0 < 0


def test_unknown_constraint_name_raises():
    """오타가 비용 함수를 조용히 그대로 두면 안 된다."""

    class Typo:
        name = "no_missing_skill"  # 끝의 s 누락
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """빈 목록은 '아무것도 채점하지 않음'이라 실수일 가능성이 높다."""
    with pytest.raises(ValueError, match="must not be empty"):
        ta.solve(build_solution(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """모듈 상위 인스턴스는 기본값 그대로여야 한다 (weight=None)."""
    assert minimize_makespan.weight is None
    assert minimize_makespan.hard is None
    assert minimize_makespan.exponent == 1.0


# --- 순수 파이썬 채점 (constraint_costs -> score) ---------------------------


def _raw():
    """한 번 푼 해의 제약별 원본 cost."""
    solution = build_solution()
    ta.solve(solution, time_limit_seconds=0.4, random_seed=1)
    return ta.constraint_costs(solution), solution


def test_score_reproduces_the_engine_hard_level():
    """기본 스펙으로 매긴 파이썬 점수의 하드 레벨은 엔진과 같아야 한다."""
    raw, solution = _raw()
    assert score(raw).hard == ta.calculate_score(solution).hard


def test_score_is_additive_over_the_picked_constraints():
    """전체 점수는 제약별 점수의 합이다 — 목록에 없는 항은 그대로 빠진다.

    하드 반올림이 unit 기여분마다 일어나므로 제약별로 나눠 더해도 값이
    같다. "이 항이 점수에 얼마를 보태는가" 를 재채점 없이 읽을 수 있다."""
    raw, _ = _raw()
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_reweighting_is_linear_without_rescoring():
    """가중치만 바꾸면 그 항이 선형으로 스케일한다 — 재채점 없이."""
    raw, _ = _raw()
    base = score(raw, [minimize_makespan])
    ten = score(raw, [MinimizeMakespan(weight=10)])
    default_weight = raw[minimize_makespan.name]["weight"]
    factor = 10 / default_weight
    if base.hard:
        assert ten.hard == pytest.approx(base.hard * factor, rel=1e-6)
    else:
        assert ten.soft == pytest.approx(base.soft * factor, rel=1e-9)


def test_score_rejects_an_unknown_constraint_name():
    """이름이 틀린 스펙(예: 이름 바뀐 컴포넌트)은 조용히 무시되면 안 된다."""
    raw, _ = _raw()

    class Renamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Renamed()])

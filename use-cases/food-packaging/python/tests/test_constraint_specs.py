"""food_packaging.constraints — 비용 함수를 선언으로 고르는 API.

제약 하나하나의 채점값 스팟체크는 `test_constraints.py` 가 맡는다. 여기서는
"어떤 제약을 켤지" 를 고르는 선택 경로만 본다.
"""

import pytest

import food_packaging as fp
from food_packaging.constraints import (
    Constraint,
    MinimizeMakespan,
    get_constraints,
    ideal_end_date_time,
    max_end_date_time,
    maximize_jobs_assigned,
    minimize_makespan,
    operator_cleaning_conflict,
    score,
)


def build_schedule():
    return fp.generate_demo_data(line_count=2, job_count=10)


def solve(constraints=None):
    schedule = build_schedule()
    fp.solve(schedule, time_limit_seconds=0.5, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """스펙이 들고 있는 이름은 코어가 실제로 아는 이름이어야 한다."""
    # unit 제약이 먼저, global 제약(세정 충돌 + 미배정)이 뒤.
    available = fp.list_constraints()
    assert available == [
        "max_end_date_time",
        "ideal_end_date_time",
        "minimize_makespan",
        "operator_cleaning_conflict",
        "maximize_jobs_assigned",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == fp.list_constraints()


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` 는 기본 경로와 같은 점수여야 한다."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain.hard == explicit.hard
    assert plain.medium == explicit.medium
    assert plain.soft == pytest.approx(explicit.soft)


def test_picking_a_subset_drops_the_other_terms():
    """목록에 없는 제약은 채점되지 않는다 — 메이크스팬 항이 통째로 사라진다."""
    baseline = solve()
    assert baseline.soft < 0

    without_makespan = solve([max_end_date_time, ideal_end_date_time, maximize_jobs_assigned])
    assert without_makespan.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """메이크스팬 가중치만 올리면 소프트만 커지고 하드는 그대로."""
    picks = [max_end_date_time, maximize_jobs_assigned]
    base = solve([*picks, minimize_makespan])
    heavier = solve([*picks, MinimizeMakespan(weight=1000)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # 벌점이므로 더 음수


def test_level_can_be_flipped():
    """레벨을 올리면 솔버의 트레이드오프가 뒤집힌다.

    메이크스팬이 소프트면 작업을 라인에 올리고 그 대가를 소프트로 치른다.
    하드로 올리면 라인을 돌리는 것 자체가 하드 비용이 되고 미배정은 미디엄에
    그치므로 — 레벨이 사전식으로 우선하니 — 솔버는 라인을 비워 둔다."""
    picks = [maximize_jobs_assigned]

    soft_level = solve([*picks, minimize_makespan])
    assert soft_level.soft < 0  # 작업을 올리고 메이크스팬을 문다

    hard_level = solve([*picks, MinimizeMakespan(hard=True)])
    assert hard_level.medium < 0  # 대신 작업을 안 올린다
    assert hard_level.soft == 0
    assert hard_level.hard == 0  # 라인을 안 돌리니 하드 비용도 0


def test_unknown_constraint_name_raises():
    """오타가 비용 함수를 조용히 그대로 두면 안 된다."""

    class Typo:
        name = "operator_cleaning_conflicts"  # 뒤에 s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """빈 목록은 '아무것도 채점하지 않음'이라 실수일 가능성이 높다."""
    with pytest.raises(ValueError, match="must not be empty"):
        fp.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """모듈 상위 인스턴스는 기본값 그대로여야 한다 (weight=None)."""
    assert operator_cleaning_conflict.weight is None
    assert operator_cleaning_conflict.hard is None
    assert operator_cleaning_conflict.exponent == 1.0


# --- 순수 파이썬 채점 (constraint_costs -> score) ---------------------------


def _raw():
    """한 번 푼 해의 제약별 원본 cost."""
    schedule = build_schedule()
    fp.solve(schedule, time_limit_seconds=0.4, random_seed=1)
    return fp.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_hard_level():
    """기본 스펙으로 매긴 파이썬 점수의 하드 레벨은 엔진과 같아야 한다."""
    raw, solution = _raw()
    assert score(raw).hard == fp.calculate_score(solution).hard


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

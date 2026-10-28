"""bed_allocation.constraints — 비용 함수를 선언으로 고르는 API.

제약 하나하나의 채점값 스팟체크는 `test_constraints.py` 가 맡는다. 여기서는
"어떤 제약을 켤지" 를 고르는 선택 경로만 본다.
"""

import pytest

import bed_allocation as ba
from bed_allocation.constraints import (
    Constraint,
    PreferredMaximumRoomCapacity,
    SameBedInSameNight,
    assign_every_patient_to_a_bed,
    get_constraints,
    preferred_maximum_room_capacity,
    same_bed_in_same_night,
    score,
)


def make_stay(index, bed=-1):
    return ba.Stay(
        f"s{index}",
        f"patient-{index}",
        ba.Gender.MALE,
        30,
        0,
        5,  # 6박
        "default",
        patient_preferred_maximum_room_capacity=1,
        bed=bed,
    )


def build_plan():
    """정원 2 병실뿐이라 선호 정원(1) 위반이 반드시 남고, 침대(3)보다 재원(4)이
    많아 "침대 없음" 미디엄 비용도 반드시 남는 배치."""
    return ba.BedPlan(
        [ba.Department("d0", "Dept", {"default": 1})],
        [ba.Room("r0", "r0", capacity=2), ba.Room("r1", "r1", capacity=2)],
        [ba.Bed("b0", 0), ba.Bed("b1", 0), ba.Bed("b2", 1)],
        [make_stay(i) for i in range(4)],
    )


def build_crowded_plan():
    """침대 1개에 겹치는 재원 2명 — 공유(하드)와 미배정(미디엄) 중 하나를
    반드시 고르게 되는 배치. 선호 정원은 비워 소프트 잡음을 없앤다."""
    return ba.BedPlan(
        [ba.Department("d0", "Dept", {"default": 1})],
        [ba.Room("r0", "r0", capacity=1)],
        [ba.Bed("b0", 0)],
        [ba.Stay(f"s{i}", f"patient-{i}", ba.Gender.MALE, 30, 0, 5, "default") for i in range(2)],
    )


def solve(constraints=None):
    plan = build_plan()
    ba.solve(plan, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return plan.score


def solve_crowded(constraints=None):
    plan = build_crowded_plan()
    ba.solve(plan, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return plan.score


def test_list_constraints_matches_the_spec_names():
    """스펙이 들고 있는 이름은 코어가 실제로 아는 이름이어야 한다."""
    # unit 제약이 먼저, global 제약이 뒤 — 글로벌인
    # different_gender_... 가 맨 뒤로 간다.
    available = ba.list_constraints()
    assert available == [
        "same_bed_in_same_night",
        "female_in_male_room",
        "male_in_female_room",
        "department_minimum_age",
        "department_maximum_age",
        "required_patient_equipment",
        "assign_every_patient_to_a_bed",
        "preferred_maximum_room_capacity",
        "department_specialty",
        "department_specialty_not_first_priority",
        "preferred_patient_equipment",
        "different_gender_in_same_gender_room_in_same_night",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == ba.list_constraints()


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` 는 기본 경로와 같은 점수여야 한다."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain.hard == explicit.hard
    assert plain.medium == explicit.medium
    assert plain.soft == pytest.approx(explicit.soft)


def test_picking_a_subset_drops_the_other_terms():
    """목록에 없는 제약은 채점되지 않는다 — 미디엄/소프트 항이 통째로 사라진다."""
    baseline = solve()
    assert baseline.medium < 0 and baseline.soft < 0

    hard_only = solve([same_bed_in_same_night])
    assert hard_only.medium == 0
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """선호 정원 가중치만 올리면 소프트만 커지고 하드는 그대로.

    `assign_every_patient_to_a_bed` 를 같이 켜 둬야 한다 — 안 켜면 미배정이
    공짜라 솔버가 전원을 침대 밖에 두고 소프트 항 자체가 사라진다."""
    picks = [same_bed_in_same_night, assign_every_patient_to_a_bed]
    base = solve([*picks, preferred_maximum_room_capacity])
    heavier = solve([*picks, PreferredMaximumRoomCapacity(weight=80)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # 벌점이므로 더 음수


def test_level_can_be_flipped():
    """하드 제약을 소프트로 내리면 솔버의 선택이 뒤집힌다.

    침대 1개에 겹치는 재원 2명: 같은 침대를 쓰면 same_bed_in_same_night,
    한 명을 빼면 assign_every_patient_to_a_bed 다. 전자가 하드면 미배정이
    싸고(레벨이 우선), 소프트로 내리면 공유가 싸진다."""
    plan_picks = [assign_every_patient_to_a_bed]

    hard_default = solve_crowded([same_bed_in_same_night, *plan_picks])
    assert hard_default.medium < 0  # 공유 대신 한 명을 빼 둔다
    assert hard_default.soft == 0

    flipped = solve_crowded([SameBedInSameNight(hard=False), *plan_picks])
    assert flipped.medium == 0  # 이제 공유가 싸다
    assert flipped.soft < 0


def test_unknown_constraint_name_raises():
    """오타가 비용 함수를 조용히 그대로 두면 안 된다."""

    class Typo:
        name = "same_bed_in_same_nights"  # 뒤에 s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """빈 목록은 '아무것도 채점하지 않음'이라 실수일 가능성이 높다."""
    with pytest.raises(ValueError, match="must not be empty"):
        ba.solve(build_plan(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """모듈 상위 인스턴스는 기본값 그대로여야 한다 (weight=None)."""
    assert preferred_maximum_room_capacity.weight is None
    assert preferred_maximum_room_capacity.hard is None
    assert preferred_maximum_room_capacity.exponent == 1.0


# --- 순수 파이썬 채점 (constraint_costs -> score) ---------------------------


def _raw():
    """한 번 푼 해의 제약별 원본 cost."""
    plan = build_plan()
    ba.solve(plan, time_limit_seconds=0.3, random_seed=1)
    return ba.constraint_costs(plan), plan


def test_score_reproduces_the_engine_hard_level():
    """기본 스펙으로 매긴 파이썬 점수의 하드 레벨은 엔진과 같아야 한다."""
    raw, solution = _raw()
    assert score(raw).hard == ba.calculate_score(solution).hard


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
    base = score(raw, [same_bed_in_same_night])
    ten = score(raw, [SameBedInSameNight(weight=10)])
    default_weight = raw[same_bed_in_same_night.name]["weight"]
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

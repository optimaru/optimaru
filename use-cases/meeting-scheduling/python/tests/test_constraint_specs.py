"""meeting_scheduling.constraints — 비용 함수를 선언으로 고르는 API.

제약 하나하나의 채점값 스팟체크는 `test_constraints.py` 가 맡는다. 여기서는
"어떤 제약을 켤지" 를 고르는 선택 경로만 본다.
"""

import pytest

import meeting_scheduling as ms
from meeting_scheduling.constraints import (
    Constraint,
    OverlappingMeetings,
    avoid_overtime,
    do_meetings_as_soon_as_possible,
    get_constraints,
    overlapping_meetings,
    required_attendance_conflict,
    required_room_capacity,
    room_conflict,
    score,
    start_and_end_on_same_day,
)

HARD_PICKS = [
    avoid_overtime,
    required_room_capacity,
    start_and_end_on_same_day,
    room_conflict,
    required_attendance_conflict,
]


def build_schedule():
    return ms.demo_data()


def solve(constraints=None):
    schedule = build_schedule()
    ms.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """스펙이 들고 있는 이름은 코어가 실제로 아는 이름이어야 한다."""
    # unit 제약이 먼저, global 제약이 뒤 — emplace 순서와 다르다.
    available = ms.list_constraints()
    assert available == [
        "avoid_overtime",
        "required_room_capacity",
        "start_and_end_on_same_day",
        "do_meetings_as_soon_as_possible",
        "assign_larger_rooms_first",
        "room_conflict",
        "required_attendance_conflict",
        "required_and_preferred_attendance_conflict",
        "preferred_attendance_conflict",
        "one_break_between_consecutive_meetings",
        "overlapping_meetings",
        "room_stability",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == ms.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """기본 스펙 전부로 조립한 비용 함수는 모놀리식 채점과 같은 값을 내야 한다."""
    schedule = build_schedule()
    ms.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=get_constraints())
    recomputed = ms.calculate_score(schedule)
    assert schedule.score.hard == recomputed.hard
    assert schedule.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """목록에 없는 제약은 채점되지 않는다 — 소프트 항이 통째로 사라진다."""
    baseline = solve()
    assert baseline.soft < 0

    hard_only = solve(HARD_PICKS)
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """겹침 가중치만 올리면 소프트만 커지고 하드는 그대로."""
    base = solve([*HARD_PICKS, overlapping_meetings])
    heavier = solve([*HARD_PICKS, OverlappingMeetings(weight=10_000)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # 벌점이므로 더 음수


def test_level_can_be_flipped():
    """소프트 제약을 하드로 올리면 그 벌점이 하드 레벨로 옮겨간다.

    회의는 달력 어딘가에 놓여야 하므로 마지막 grain 인덱스 합은 0 이 될 수
    없다 — 레벨을 올리면 반드시 하드에 잡힌다."""
    soft_level = solve([*HARD_PICKS, do_meetings_as_soon_as_possible])
    assert soft_level.hard == 0 and soft_level.soft < 0

    hard_level = solve([*HARD_PICKS, type(do_meetings_as_soon_as_possible)(hard=True)])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """오타가 비용 함수를 조용히 그대로 두면 안 된다."""

    class Typo:
        name = "room_conflicts"  # 뒤에 s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """빈 목록은 '아무것도 채점하지 않음'이라 실수일 가능성이 높다."""
    with pytest.raises(ValueError, match="must not be empty"):
        ms.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """모듈 상위 인스턴스는 기본값 그대로여야 한다 (weight=None)."""
    assert overlapping_meetings.weight is None
    assert overlapping_meetings.hard is None
    assert overlapping_meetings.exponent == 1.0


# --- 순수 파이썬 채점 (constraint_costs -> score) ---------------------------


def _raw():
    """한 번 푼 해의 제약별 원본 cost."""
    schedule = build_schedule()
    ms.solve(schedule, time_limit_seconds=0.4, random_seed=1)
    return ms.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_score_exactly():
    """파이썬 채점은 엔진과 hard/soft 가 정확히 같아야 한다.

    이 도메인은 레벨이 둘뿐이라 접힘 없이 그대로 견줄 수 있다. 이 단언이
    `score()` 가 엔진의 applyRaw 산술(가중치·셰이핑·unit 별 하드 반올림)을
    제대로 재현하는지 이 패키지에서 고정한다."""
    raw, solution = _raw()
    engine = ms.calculate_score(solution)
    python = score(raw)
    assert python.hard == engine.hard
    assert python.soft == pytest.approx(engine.soft)


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
    base = score(raw, [overlapping_meetings])
    ten = score(raw, [OverlappingMeetings(weight=10)])
    default_weight = raw[overlapping_meetings.name]["weight"]
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

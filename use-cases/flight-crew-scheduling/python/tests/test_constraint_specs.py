"""flight_crew.constraints — 비용 함수를 선언으로 고르는 API.

제약 하나하나의 채점값 스팟체크는 `test_constraints.py` 가 맡는다. 여기서는
"어떤 제약을 켤지" 를 고르는 선택 경로만 본다.
"""

import pytest

import flight_crew as fc
from flight_crew.constraints import (
    Constraint,
    FlightConflict,
    RequiredSkill,
    employee_unavailability,
    first_assignment_not_departing_from_home,
    flight_conflict,
    get_constraints,
    last_assignment_not_arriving_at_home,
    required_skill,
    score,
)

HOUR = 60  # 분 단위


def build_schedule():
    """NRT 소속 조종사 2명인데 왕복은 전부 ICN 에서 시작해 ICN 으로 끝난다.

    그래서 어떤 배정을 해도 첫 출발과 마지막 도착이 홈 공항이 아니고, 두
    소프트 항이 반드시 남는다 — 항을 빼거나 값을 바꿨을 때의 차이가 보이는
    작은 인스턴스다.
    """
    airports = [
        fc.Airport("ICN", "Incheon", 37.46, 126.44),
        fc.Airport("NRT", "Narita", 35.77, 140.39),
    ]
    employees = [
        fc.Employee("P1", "Pilot 1", home_airport=1, skills=["Pilot"]),
        fc.Employee("P2", "Pilot 2", home_airport=1, skills=["Pilot"]),
        fc.Employee("X1", "No skill", home_airport=1, skills=[]),
    ]
    # 하루차: ICN->NRT, NRT->ICN. 이튿날: 같은 왕복 한 번 더.
    flights = [
        fc.Flight("KE001", 0, 9 * HOUR, 1, 11 * HOUR),
        fc.Flight("KE002", 1, 13 * HOUR, 0, 15 * HOUR),
        fc.Flight("KE003", 0, 33 * HOUR, 1, 35 * HOUR),
        fc.Flight("KE004", 1, 37 * HOUR, 0, 39 * HOUR),
    ]
    assignments = [fc.FlightAssignment(f"a{i}", i, required_skill="Pilot") for i in range(4)]
    return fc.FlightCrewSchedule(airports, employees, flights, assignments)


def solve(constraints=None):
    schedule = build_schedule()
    fc.solve(schedule, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """스펙이 들고 있는 이름은 코어가 실제로 아는 이름이어야 한다."""
    available = fc.list_constraints()
    assert available == [
        "required_skill",
        "flight_conflict",
        "transfer_between_two_flights",
        "employee_unavailability",
        "first_assignment_not_departing_from_home",
        "last_assignment_not_arriving_at_home",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == fc.list_constraints()


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` 는 기본 경로와 같은 점수여야 한다."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain.hard == explicit.hard
    assert plain.soft == pytest.approx(explicit.soft)


def test_picking_a_subset_drops_the_other_terms():
    """목록에 없는 제약은 채점되지 않는다 — 홈 공항 항이 통째로 사라진다."""
    baseline = solve()
    assert baseline.soft < 0

    hard_only = solve([required_skill, flight_conflict, employee_unavailability])
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """스킬 가중치만 올리면 하드가 커지되 소프트 구성은 그대로."""
    picks = [first_assignment_not_departing_from_home]
    base = solve([*picks, required_skill])
    heavier = solve([*picks, RequiredSkill(weight=10_000)])
    assert heavier.hard <= base.hard  # 벌점이므로 같거나 더 음수


def test_level_can_be_flipped():
    """하드 제약을 소프트로 내리면 그 벌점이 소프트 레벨로 옮겨간다."""
    hard_level = solve([flight_conflict, last_assignment_not_arriving_at_home])
    soft_level = solve([FlightConflict(hard=False), last_assignment_not_arriving_at_home])
    # 같은 비용이 어느 레벨에 실리는지만 다르다.
    assert hard_level.hard <= 0
    assert soft_level.hard == 0


def test_unknown_constraint_name_raises():
    """오타가 비용 함수를 조용히 그대로 두면 안 된다."""

    class Typo:
        name = "required_skills"  # 뒤에 s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """빈 목록은 '아무것도 채점하지 않음'이라 실수일 가능성이 높다."""
    with pytest.raises(ValueError, match="must not be empty"):
        fc.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """모듈 상위 인스턴스는 기본값 그대로여야 한다 (weight=None)."""
    assert required_skill.weight is None
    assert required_skill.hard is None
    assert required_skill.exponent == 1.0


# --- 순수 파이썬 채점 (constraint_costs -> score) ---------------------------


def _raw():
    """한 번 푼 해의 제약별 원본 cost."""
    schedule = build_schedule()
    fc.solve(schedule, time_limit_seconds=0.3, random_seed=1)
    return fc.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_score_exactly():
    """파이썬 채점은 엔진과 hard/soft 가 정확히 같아야 한다.

    이 도메인은 레벨이 둘뿐이라 접힘 없이 그대로 견줄 수 있다. 이 단언이
    `score()` 가 엔진의 applyRaw 산술(가중치·셰이핑·unit 별 하드 반올림)을
    제대로 재현하는지 이 패키지에서 고정한다."""
    raw, solution = _raw()
    engine = fc.calculate_score(solution)
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
    base = score(raw, [required_skill])
    ten = score(raw, [RequiredSkill(weight=10)])
    default_weight = raw[required_skill.name]["weight"]
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

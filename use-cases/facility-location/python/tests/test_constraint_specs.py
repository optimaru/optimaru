"""facility_location.constraints — 비용 함수를 선언으로 고르는 API."""

import pytest

import facility_location as fl
from facility_location.constraints import (
    Constraint,
    DistanceFromFacility,
    FacilityCapacity,
    capacity,
    distance,
    get_constraints,
    score,
    setup_cost,
)


def build_problem():
    """설치비 싼 먼 시설 vs 비싼 가까운 시설. 어떤 제약을 켜느냐로 답이 갈린다."""
    return fl.FacilityLocationProblem(
        [
            fl.Facility("near", fl.Location(0.0, 0.0), 1000, 5),
            fl.Facility("far", fl.Location(0.1, 0.0), 10, 100),
        ],
        [fl.Consumer(f"C{i}", fl.Location(0.0, 0.0), 3) for i in range(4)],
    )


def solve(constraints=None):
    problem = build_problem()
    fl.solve(problem, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return problem.score


def test_list_constraints_matches_the_spec_names():
    """스펙이 들고 있는 이름은 코어가 실제로 아는 이름이어야 한다."""
    available = fl.list_constraints()
    assert available == [
        "facility_capacity",
        "facility_setup_cost",
        "distance_from_facility",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == fl.list_constraints()


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` 는 기본 경로와 같은 점수여야 한다."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain.hard == explicit.hard
    assert plain.soft == pytest.approx(explicit.soft)


def test_picking_a_subset_drops_the_other_terms():
    """목록에 없는 제약은 채점되지 않는다 — 소프트 항이 통째로 사라진다."""
    hard_only = solve([capacity])
    assert hard_only.soft == 0.0


def test_reweighting_one_constraint_scales_only_that_term():
    """거리 가중치만 10배: 소프트가 커지되 하드는 그대로."""
    base = solve([capacity, distance])
    heavier = solve([capacity, DistanceFromFacility(weight=60)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # 벌점이므로 더 음수


def test_level_can_be_flipped():
    """하드 제약을 소프트로 내리면 하드 레벨이 비워진다."""
    problem = build_problem()
    # 용량 5짜리 시설에 수요 12를 몰아넣어 확실히 위반시킨다.
    for consumer in problem.consumers:
        consumer.facility = 0
    soft_capacity = FacilityCapacity(hard=False)
    assert fl.calculate_score(problem).hard < 0
    fl.solve(problem, time_limit_seconds=0.2, random_seed=1, constraints=[soft_capacity])
    assert problem.score.hard == 0


def test_unknown_constraint_name_raises():
    """오타가 비용 함수를 조용히 그대로 두면 안 된다."""

    class Typo:
        name = "facility_setup_costs"  # 뒤에 s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """빈 목록은 '아무것도 채점하지 않음'이라 실수일 가능성이 높다."""
    with pytest.raises(ValueError, match="must not be empty"):
        fl.solve(build_problem(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_setup_cost_instance_is_the_default_weight():
    """모듈 상위 인스턴스는 기본값 그대로여야 한다 (weight=None)."""
    assert setup_cost.weight is None
    assert setup_cost.hard is None
    assert setup_cost.exponent == 1.0


def test_constraint_costs_match_capacity_setup_and_distance_by_hand():
    """Each raw component is checked without running a solver.

    Facility 0 carries demand 4 + 3 against capacity 5, is opened once for
    100, and serves consumers 0.003 and 0.004 degrees away.  With the domain's
    111,000 m/degree metric those distances are exactly 333 m and 444 m.
    The unassigned consumer and unused facility contribute nothing.
    """
    problem = fl.FacilityLocationProblem(
        [
            fl.Facility("used", fl.Location(0.0, 0.0), 100, 5),
            fl.Facility("unused", fl.Location(1.0, 1.0), 999, 100),
        ],
        [
            fl.Consumer("c1", fl.Location(0.003, 0.0), 4),
            fl.Consumer("c2", fl.Location(0.0, 0.004), 3),
            fl.Consumer("unassigned", fl.Location(1.0, 1.0), 50),
        ],
    )
    problem.set_assignment([0, 0, -1])

    raw = fl.constraint_costs(problem)

    assert raw["facility_capacity"]["costs"] == pytest.approx([2.0, 0.0])
    assert raw["facility_setup_cost"]["costs"] == pytest.approx([100.0, 0.0])
    assert raw["distance_from_facility"]["costs"] == pytest.approx([777.0, 0.0])
    # Default weights: capacity=1 hard, setup=2 soft, distance=6 soft.
    assert score(raw).hard == -2
    assert score(raw).soft == pytest.approx(-(2 * 100 + 6 * 777))


# --- 순수 파이썬 채점 (constraint_costs -> score) ---------------------------


def _solved(constraints=None):
    problem = build_problem()
    fl.solve(problem, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return problem


def test_score_equals_the_engine_for_the_default_assembly():
    """레벨이 둘뿐인 도메인이라 하드/소프트를 그대로 대조할 수 있다."""
    problem = _solved()
    engine = fl.calculate_score(problem)
    python = score(fl.constraint_costs(problem))
    assert python.hard == engine.hard
    assert python.soft == pytest.approx(engine.soft)


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [capacity],
        [capacity, setup_cost, DistanceFromFacility(weight=60)],
        [FacilityCapacity(hard=False), setup_cost, distance],
        [capacity, setup_cost, DistanceFromFacility(exponent=2.0)],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """임의 스펙으로 푼 해를 그 스펙으로 재계산하면 엔진 점수와 같아야 한다.

    exponent 가 unit 별로 적용되므로 unit 별 원본 cost 를 받아야 재현된다 —
    합계만으로는 안 된다."""
    problem = _solved(specs)
    python = score(fl.constraint_costs(problem), specs)
    assert python.hard == problem.score.hard
    assert python.soft == pytest.approx(problem.score.soft)


def test_score_is_additive_over_the_picked_constraints():
    problem = _solved()
    raw = fl.constraint_costs(problem)
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_rejects_an_unknown_constraint_name():
    """이름이 틀린 스펙(예: 이름 바뀐 컴포넌트)은 조용히 무시되면 안 된다."""
    raw = fl.constraint_costs(_solved())

    class Renamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Renamed()])


def test_naming_a_constraint_twice_is_rejected():
    """같은 제약을 두 번 넘기면 뒤엣것이 조용히 이기면 안 된다.

    오타와 같은 부류의 실수라 에러다 — 원하는 메타데이터로 한 번만 적어야
    한다."""
    with pytest.raises(ValueError, match="picked more than once"):
        fl.solve(
            build_problem(),
            time_limit_seconds=0.2,
            constraints=[capacity, DistanceFromFacility(weight=10), distance],
        )

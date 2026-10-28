"""vrp_routing.constraints — 비용 함수를 선언으로 고르는 API.

"어떤 제약을 켤지" 를 고르는 선택 경로만 본다. 제약별 채점값은
`test_routing.py` 와 C++ 테스트가 맡는다.
"""

import pytest

import vrp_routing as vrp
from vrp_routing.constraints import (
    Constraint,
    MinimizeTravelTime,
    score,
    get_constraints,
    maximize_visits_assigned,
    minimize_travel_time,
    vehicle_capacity,
)


def small_plan():
    """2 vehicles / 6 visits around Seoul; capacities force a split."""
    locations = [
        vrp.Location(37.5665, 126.9780),  # 0: V1 home
        vrp.Location(37.4979, 127.0276),  # 1: V2 home
    ]
    visit_coords = [
        (37.5700, 126.9820),
        (37.5610, 126.9905),
        (37.5512, 126.9882),
        (37.5040, 127.0250),
        (37.5133, 127.1001),
        (37.4901, 127.0330),
    ]
    visits = []
    for i, (lat, lon) in enumerate(visit_coords):
        locations.append(vrp.Location(lat, lon))
        visits.append(
            vrp.Visit(f"C{i}", f"Customer {i}", 2 + i, 2, 8 * 3600, 20 * 3600, 600)
        )
    vehicles = [
        vrp.Vehicle("V1", 6, 0, departure_time=7 * 3600),
        vrp.Vehicle("V2", 6, 1, departure_time=7 * 3600),
    ]
    return vrp.VehicleRoutePlan("test", locations, vehicles, visits)


def solve(constraints=None, seconds=0.4, seed=7):
    plan = small_plan()
    cfg = vrp.SolverConfig()
    cfg.spent_limit_seconds = seconds
    cfg.random_seed = seed
    vrp.Solver(cfg).solve_with_constraints(plan, constraints)
    return plan


def test_list_constraints_matches_the_spec_names():
    """스펙이 들고 있는 이름은 코어가 실제로 아는 이름이어야 한다."""
    # unit 제약이 먼저, global 제약(maximize_visits_assigned)이 뒤.
    available = vrp.list_constraints()
    assert available == [
        "vehicle_capacity",
        "service_finished_after_max_end_time",
        "minimize_travel_time",
        "max_travel_time",
        "max_route_duration",
        "vehicle_end_time",
        "max_stops",
        "max_route_radius",
        "minimize_enclosing_circle_radius",
        "min_capacity_utilization",
        "minimize_route_duration",
        "minimize_latency",
        "vehicle_fixed_cost",
        "maximize_visits_assigned",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == vrp.list_constraints()


def test_constraint_catalog_copy_is_bilingual():
    for spec in get_constraints():
        constraint_type = type(spec)
        assert constraint_type.label_ko
        assert constraint_type.label_en
        assert constraint_type.description_ko
        assert constraint_type.description_en


def test_route_policy_costs_are_exposed():
    plan = small_plan()
    vehicle = plan.vehicles[0]
    vehicle.visit_indices = [0, 1, 2]
    vehicle.max_stops = 2
    vehicle.max_travel_time = 1
    vehicle.max_route_duration = 1
    vehicle.max_end_time = vehicle.departure_time + 1
    vehicle.fixed_cost = 25
    vehicle.max_route_radius_meters = 1
    plan.rebuild_unassigned_pool()
    costs = vrp.constraint_costs(plan)
    assert costs["max_stops"]["costs"][0] == 1
    assert costs["max_travel_time"]["costs"][0] > 0
    assert costs["max_route_duration"]["costs"][0] > 0
    assert costs["vehicle_end_time"]["costs"][0] > 0
    assert costs["vehicle_fixed_cost"]["costs"][0] == 25
    assert costs["max_route_radius"]["costs"][0] > 0


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """기본 스펙 전부로 조립한 비용 함수는 모놀리식 채점과 같은 값을 내야 한다."""
    plan = solve(get_constraints())
    recomputed = vrp.calculate_score(plan)
    assert plan.score.hard == recomputed.hard
    assert plan.score.medium == recomputed.medium
    assert plan.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """목록에 없는 제약은 채점되지 않는다 — 주행시간 항이 통째로 사라진다."""
    baseline = solve(get_constraints()).score
    assert baseline.soft < 0

    without_travel = solve([vehicle_capacity, maximize_visits_assigned]).score
    assert without_travel.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """주행시간 가중치만 올리면 소프트만 커지고 하드는 그대로."""
    picks = [vehicle_capacity, maximize_visits_assigned]
    base = solve([*picks, minimize_travel_time]).score
    heavier = solve([*picks, MinimizeTravelTime(weight=10)]).score
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # 벌점이므로 더 음수


def test_level_can_be_flipped():
    """레벨을 올리면 솔버의 트레이드오프가 뒤집힌다.

    주행시간이 소프트면 방문을 다 돌고 그 대가를 소프트로 치른다. 하드로
    올리면 주행 자체가 하드 비용이 되고 미배정은 미디엄에 그치므로 —
    레벨이 사전식으로 우선하니 — 솔버는 아예 차를 세워 둔다."""
    picks = [maximize_visits_assigned]

    soft_level = solve([*picks, minimize_travel_time]).score
    assert soft_level.medium == 0  # 방문을 다 돈다
    assert soft_level.soft < 0

    hard_level = solve([*picks, MinimizeTravelTime(hard=True)]).score
    assert hard_level.medium < 0  # 대신 방문을 포기한다
    assert hard_level.soft == 0
    assert hard_level.hard == 0  # 달리지 않으니 하드 비용도 0


def test_unknown_constraint_name_raises():
    """오타가 비용 함수를 조용히 그대로 두면 안 된다."""

    class Typo:
        name = "minimize_travel_times"  # 뒤에 s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """빈 목록은 '아무것도 채점하지 않음'이라 실수일 가능성이 높다."""
    with pytest.raises(ValueError, match="must not be empty"):
        solve([])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """모듈 상위 인스턴스는 기본값 그대로여야 한다 (weight=None)."""
    assert minimize_travel_time.weight is None
    assert minimize_travel_time.hard is None
    assert minimize_travel_time.exponent == 1.0


# --- 순수 파이썬 채점 (constraint_costs -> score) ---------------------------


def _raw():
    """한 번 푼 해의 제약별 원본 cost."""
    plan = solve(get_constraints())
    return vrp.constraint_costs(plan), plan


def test_score_reproduces_the_engine_hard_level():
    """기본 스펙으로 매긴 파이썬 점수의 하드 레벨은 엔진과 같아야 한다."""
    raw, solution = _raw()
    assert score(raw).hard == vrp.calculate_score(solution).hard


def test_score_is_additive_over_the_picked_constraints():
    """전체 점수는 제약별 점수의 합이다 — 목록에 없는 항은 그대로 빠진다."""
    raw, _ = _raw()
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_reweighting_is_linear_without_rescoring():
    """가중치만 바꾸면 그 항이 선형으로 스케일한다 — 재채점 없이."""
    raw, _ = _raw()
    base = score(raw, [minimize_travel_time])
    ten = score(raw, [MinimizeTravelTime(weight=10)])
    factor = 10 / raw[minimize_travel_time.name]["weight"]
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

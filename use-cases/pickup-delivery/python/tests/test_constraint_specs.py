"""pdp_routing.constraints — an API for picking the cost function declaratively.

"Which constraints are on" is the only path covered here. Per-constraint values
are covered by `test_pdp.py` and the C++ tests.
"""

import pdp_routing as pdp
import pytest
from pdp_routing.constraints import (
    Constraint,
    MinimizeTravelTime,
    score,
    get_constraints,
    minimize_travel_time,
    paired_request,
    same_vehicle,
    unassigned_requests,
    vehicle_capacity,
)


def hour_minute(hour: int, minute: int) -> int:
    return hour * 3600 + minute * 60


def build_plan(demand: int = 4, capacity: int = 10) -> pdp.PdpRoutePlan:
    """One vehicle, two requests — the same tiny instance test_pdp.py uses."""
    locations = [
        pdp.Location(43.77, 11.25),  # 0 depot
        pdp.Location(43.78, 11.26),  # 1 pickup 1
        pdp.Location(43.79, 11.27),  # 2 dropoff 1
        pdp.Location(43.76, 11.24),  # 3 pickup 2
        pdp.Location(43.75, 11.23),  # 4 dropoff 2
    ]
    open_, close = hour_minute(8, 0), hour_minute(18, 0)
    nodes = [
        pdp.Node("p1", 1, open_, close, 300, 0, True),
        pdp.Node("d1", 2, open_, close, 300, 0, False),
        pdp.Node("p2", 3, open_, close, 300, 1, True),
        pdp.Node("d2", 4, open_, close, 300, 1, False),
    ]
    deliveries = [
        pdp.Delivery("r1", 0, 1, demand),
        pdp.Delivery("r2", 2, 3, demand),
    ]
    vehicles = [pdp.Vehicle("1", capacity, 0, hour_minute(7, 0))]
    return pdp.PdpRoutePlan("tiny", locations, vehicles, nodes, deliveries)


def solve(constraints=None, seconds=0.4, seed=1):
    plan = build_plan()
    cfg = pdp.SolverConfig()
    cfg.spent_limit_seconds = seconds
    cfg.random_seed = seed
    pdp.Solver(cfg).solve_with_constraints(plan, constraints)
    return plan


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    # Unit constraints first, global constraints (the three coupling terms + unassigned) after.
    available = pdp.list_constraints()
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
        "paired_request",
        "same_vehicle",
        "pickup_before_dropoff",
        "unassigned_requests",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == pdp.list_constraints()


def test_route_policy_costs_are_exposed():
    plan = build_plan()
    vehicle = plan.vehicles[0]
    vehicle.node_indices = [0, 1, 2, 3]
    vehicle.max_stops = 2
    vehicle.max_travel_time = 1
    vehicle.max_route_duration = 1
    vehicle.max_end_time = vehicle.departure_time + 1
    vehicle.fixed_cost = 25
    vehicle.max_route_radius_meters = 1
    plan.rebuild_unassigned_pool()
    costs = pdp.constraint_costs(plan)
    assert costs["max_stops"]["costs"][0] == 2
    assert costs["max_travel_time"]["costs"][0] > 0
    assert costs["max_route_duration"]["costs"][0] > 0
    assert costs["vehicle_end_time"]["costs"][0] > 0
    assert costs["vehicle_fixed_cost"]["costs"][0] == 25
    assert costs["max_route_radius"]["costs"][0] > 0


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """A cost function assembled from all default specs must equal the monolithic scoring."""
    plan = solve(get_constraints())
    recomputed = pdp.calculate_score(plan)
    assert plan.score.hard == recomputed.hard
    assert plan.score.medium == recomputed.medium
    assert plan.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the travel-time term disappears entirely."""
    baseline = solve(get_constraints()).score
    assert baseline.soft < 0

    without_travel = solve([vehicle_capacity, paired_request, same_vehicle]).score
    assert without_travel.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the travel-time weight grows soft only; hard stays put."""
    picks = [vehicle_capacity, paired_request, same_vehicle, unassigned_requests]
    base = solve([*picks, minimize_travel_time]).score
    heavier = solve([*picks, MinimizeTravelTime(weight=10)]).score
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Raising the level flips the solver's trade-off.

    With soft travel time, every request gets carried and the cost is paid in soft.
    Promoted to hard, driving itself becomes a hard cost while unassigned stays
    medium — levels are lexicographic — so the solver parks the vehicles."""
    picks = [paired_request, same_vehicle, unassigned_requests]

    soft_level = solve([*picks, minimize_travel_time]).score
    assert soft_level.medium == 0  # carries every request
    assert soft_level.soft < 0

    hard_level = solve([*picks, MinimizeTravelTime(hard=True)]).score
    assert hard_level.medium < 0  # gives up requests instead
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "same_vehicles"  # trailing s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        solve([])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None)."""
    assert minimize_travel_time.weight is None
    assert minimize_travel_time.hard is None
    assert minimize_travel_time.exponent == 1.0


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    plan = solve(get_constraints())
    return pdp.constraint_costs(plan), plan


def test_score_reproduces_the_engine_hard_level():
    """The hard level of the Python score at default specs must match the engine."""
    raw, solution = _raw()
    assert score(raw).hard == pdp.calculate_score(solution).hard


def test_score_is_additive_over_the_picked_constraints():
    """The total score is the sum of per-constraint scores — unlisted terms simply drop out."""
    raw, _ = _raw()
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_reweighting_is_linear_without_rescoring():
    """Changing only the weight scales that term linearly — no rescoring."""
    raw, _ = _raw()
    base = score(raw, [minimize_travel_time])
    ten = score(raw, [MinimizeTravelTime(weight=10)])
    factor = 10 / raw[minimize_travel_time.name]["weight"]
    if base.hard:
        assert ten.hard == pytest.approx(base.hard * factor, rel=1e-6)
    else:
        assert ten.soft == pytest.approx(base.soft * factor, rel=1e-9)


def test_score_rejects_an_unknown_constraint_name():
    """A spec with a wrong name (e.g. a renamed component) must not be silently ignored."""
    raw, _ = _raw()

    class Renamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Renamed()])

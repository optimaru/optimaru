"""optimaru.pickup_delivery.constraints — an API for picking the cost function declaratively.

"Which constraints are on", the catalog (`constraint_info()`), the five knobs, repeated
picks and the medium level are covered here. Per-constraint values are covered by
`test_pdp.py` and the C++ tests (one hand-calculated positive and zero case each).
"""

from types import SimpleNamespace

import pytest

import optimaru.pickup_delivery as pdp
from optimaru._constraint import instance_keys
from optimaru.pickup_delivery import constraints as specs
from optimaru.pickup_delivery.constraints import (
    EnclosingCircleRadiusCost,
    LatencyCost,
    Level,
    MaxEndTimeCost,
    MaxStopsCost,
    MinCapacityUtilizationCost,
    PairedRequestCost,
    RequiredVehicleCost,
    RouteDurationCost,
    SameVehicleCost,
    SquaredOrderAgeCost,
    TravelTimeCost,
    UnassignedRequestsCost,
    VehicleCapacityCost,
    get_constraints,
    paired_request_cost,
    pickup_before_dropoff_cost,
    same_vehicle_cost,
    score,
    travel_time_cost,
    unassigned_requests_cost,
    vehicle_capacity_cost,
)

MEDIUM_UNIT = 1_000_000_000  # the domain's medium fold (into soft)


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
    # Unit constraints first, global constraints (the three coupling terms, groups, unassigned) after.
    available = pdp.list_constraints()
    assert available == [
        "vehicle_capacity_cost",
        "max_end_time_cost",
        "travel_time_cost",
        "max_travel_time_cost",
        "max_route_duration_cost",
        "vehicle_max_end_time_cost",
        "max_stops_cost",
        "max_route_radius_cost",
        "enclosing_circle_radius_cost",
        "min_capacity_utilization_cost",
        "route_duration_cost",
        "latency_cost",
        "squared_order_age_cost",
        "vehicle_fixed_cost",
        "pinned_stops_cost",
        "required_vehicle_cost",
        "paired_request_cost",
        "same_vehicle_cost",
        "pickup_before_dropoff_cost",
        "request_group_cost",
        "unassigned_requests_cost",
    ]
    for spec in get_constraints():
        assert spec.name in available


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
    assert costs["max_stops_cost"]["costs"][0] == 2
    assert costs["max_travel_time_cost"]["costs"][0] > 0
    assert costs["max_route_duration_cost"]["costs"][0] > 0
    assert costs["vehicle_max_end_time_cost"]["costs"][0] > 0
    assert costs["vehicle_fixed_cost"]["costs"][0] == 25
    assert costs["max_route_radius_cost"]["costs"][0] > 0


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

    without_travel = solve([vehicle_capacity_cost, paired_request_cost, same_vehicle_cost]).score
    assert without_travel.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the travel-time weight grows soft only; hard stays put."""
    picks = [
        vehicle_capacity_cost,
        paired_request_cost,
        same_vehicle_cost,
        unassigned_requests_cost,
    ]
    base = solve([*picks, travel_time_cost]).score
    heavier = solve([*picks, TravelTimeCost(weight=10)]).score
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Raising the level flips the solver's trade-off.

    With soft travel time, every request gets carried and the cost is paid in soft.
    Promoted to hard, driving itself becomes a hard cost while unassigned stays
    medium — levels are lexicographic — so the solver parks the vehicles."""
    picks = [paired_request_cost, same_vehicle_cost, unassigned_requests_cost]

    soft_level = solve([*picks, travel_time_cost]).score
    assert soft_level.medium == 0  # carries every request
    assert soft_level.soft < 0

    hard_level = solve([*picks, TravelTimeCost(level="hard")]).score
    assert hard_level.medium < 0  # gives up requests instead
    assert hard_level.soft == 0


def test_a_soft_constraint_can_move_to_medium():
    """On medium, travel time outranks every soft term and folds at the medium unit."""
    picks = [paired_request_cost, same_vehicle_cost, unassigned_requests_cost]
    soft_level = solve([*picks, travel_time_cost]).score
    medium_level = solve([*picks, TravelTimeCost(level=Level.MEDIUM)]).score

    assert soft_level.soft < 0
    assert medium_level.soft == 0
    assert medium_level.medium < 0


def test_core_refuses_an_unknown_level_like_the_python_base():
    pick = SimpleNamespace(name="unassigned_requests_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        solve([pick])


def test_score_agrees_with_the_engine_when_a_constraint_moves_to_medium():
    picks = [paired_request_cost, same_vehicle_cost, unassigned_requests_cost, TravelTimeCost(level="medium")]
    plan = solve(picks)
    assert score(pdp.constraint_costs(plan), picks) == _engine(plan.score)
    assert score(pdp.constraint_costs(plan, picks), picks) == _engine(plan.score)


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None, level=None, exponent=1)."""
    for spec in get_constraints():
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
        assert getattr(specs, spec.name) is spec  # the instance is named after the constraint
    # The opt-in objectives pass no explicit weight=0: the C++ default is 0.
    assert specs.enclosing_circle_radius_cost.weight is None


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
    base = score(raw, [travel_time_cost])
    ten = score(raw, [TravelTimeCost(weight=10)])
    factor = 10 / raw[travel_time_cost.name]["weight"]
    if base.hard:
        assert ten.hard == pytest.approx(base.hard * factor, rel=1e-6)
    else:
        assert ten.soft == pytest.approx(base.soft * factor, rel=1e-9)


def order_age_input(order_times: list[int]) -> pdp.models.PdpInput:
    """One vehicle at 0, and requests whose stops are all at the same place, 100 s away."""
    size = 1 + 2 * len(order_times)
    seconds = [[0 if i == j or (i and j) else 100 for j in range(size)] for i in range(size)]
    stop = lambda name: pdp.models.StopModel(id=name, location=(37.5, 127.0), max_end_time=10**6, service_duration=10)
    return pdp.models.PdpInput(
        vehicles=[pdp.models.VehicleModel(id="v", capacity=5, home=(37.5, 127.0), max_stops=0)],
        deliveries=[
            pdp.models.DeliveryModel(
                id=f"r{i}",
                demand=1,
                order_time=order,
                pickup=stop(f"p{i}"),
                dropoff=stop(f"d{i}"),
            )
            for i, order in enumerate(order_times)
        ],
        driving_seconds=seconds,
    )


def order_age_cost(model_input, route: list[int]) -> float:
    plan = pdp.models.to_plan(model_input)
    plan.vehicles[0].node_indices = route
    plan.rebuild_unassigned_pool()
    return sum(pdp.constraint_costs(plan)["squared_order_age_cost"]["costs"])


def test_order_age_measures_each_dropoff_from_its_order():
    # Pickup ends at 100 + 10; the dropoff is at the same place and ends at 120.
    assert order_age_cost(order_age_input([0]), [0, 1]) == 120**2
    assert order_age_cost(order_age_input([-300]), [0, 1]) == 420**2


def test_order_age_ignores_where_the_pickups_fall():
    # Only the dropoffs' finish times differ from a plain sum of arrivals.
    together = order_age_cost(order_age_input([0, 0]), [0, 2, 1, 3])  # finish at 130, 140
    assert together == 130**2 + 140**2


def test_order_age_squares_per_request_not_per_route():
    assert order_age_cost(order_age_input([0, 0]), [0, 1, 2, 3]) == 120**2 + 140**2


def test_an_unassigned_request_is_charged_more_the_older_it_is():
    charge = order_age_cost(order_age_input([0]), [])
    older = order_age_cost(order_age_input([-1000]), [])
    assert charge == 3600**2
    assert older == 4600**2


def test_solving_for_order_age_serves_the_oldest_order_first():
    model_input = order_age_input([0, -2000])
    replaced = {"travel_time_cost", "squared_order_age_cost"}
    constraints = [c for c in pdp.constraints.get_constraints() if c.name not in replaced]
    constraints.append(pdp.constraints.SquaredOrderAgeCost(weight=1))
    request = pdp.models.PdpRequest(model_input=model_input, timelimit=0.3)

    solution = pdp.models.solve_request(request, constraints=constraints)

    dropoffs = [stop.delivery_id for stop in solution.routes["v"] if not stop.pickup]
    assert dropoffs == ["r1", "r0"]


# --- The catalog (constraint_info) -------------------------------------------

# (name, default level, default weight, default_enabled), in list_constraints() order.
EXPECTED_INFO = [
    ("vehicle_capacity_cost", "hard", 1.0, True),
    ("max_end_time_cost", "hard", 1.0, True),
    ("travel_time_cost", "soft", 1.0, True),
    ("max_travel_time_cost", "hard", 1.0, True),
    ("max_route_duration_cost", "hard", 1.0, True),
    ("vehicle_max_end_time_cost", "hard", 1.0, True),
    ("max_stops_cost", "hard", 1.0, True),
    ("max_route_radius_cost", "hard", 1.0, True),
    ("enclosing_circle_radius_cost", "soft", 0.0, False),
    ("min_capacity_utilization_cost", "soft", 1.0, True),
    ("route_duration_cost", "soft", 0.0, False),
    ("latency_cost", "soft", 0.0, False),
    ("squared_order_age_cost", "soft", 0.0, False),
    ("vehicle_fixed_cost", "soft", 1.0, True),
    ("pinned_stops_cost", "hard", 1e12, True),
    ("required_vehicle_cost", "hard", 1000.0, True),
    ("paired_request_cost", "hard", 1000.0, True),
    ("same_vehicle_cost", "hard", 1000.0, True),
    ("pickup_before_dropoff_cost", "hard", 1000.0, True),
    ("request_group_cost", "hard", 1000.0, True),
    ("unassigned_requests_cost", "medium", 1.0, True),
]
GLOBALS = {
    "paired_request_cost",
    "same_vehicle_cost",
    "pickup_before_dropoff_cost",
    "unassigned_requests_cost",
    "request_group_cost",
}


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = pdp.constraint_info()
    assert [row.name for row in rows] == pdp.list_constraints()
    assert [(row.name, row.default_level, row.default_weight, row.default_enabled) for row in rows] == EXPECTED_INFO
    # Every constraint is flippable in the engine (the coupling rules are structural
    # on the CP-SAT path only) and none is a reward.
    assert all(row.level_flippable and not row.reward for row in rows)


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults and carries the domain's medium fold on every entry."""
    plan = build_plan()
    plan.rebuild_unassigned_pool()
    raw = pdp.constraint_costs(plan)
    assert list(raw) == pdp.list_constraints()
    for row in pdp.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"]) == (
            row.default_weight,
            row.default_level,
        )
        assert (entry["level_flippable"], entry["reward"], entry["global"]) == (
            True,
            False,
            row.name in GLOBALS,
        )
        assert (entry["name"], entry["label"]) == (row.name, "")
        assert entry["medium_fold"] == {"kind": "into_soft", "unit": float(MEDIUM_UNIT)}


# --- The knobs ------------------------------------------------------------------


def _engine(score_):
    """The engine-level (hard, soft) of a hard / medium / soft plan score (medium folded into soft)."""
    return score_.hard, score_.medium * MEDIUM_UNIT + score_.soft


def _half_served_plan():
    """r1 routed, r2 (2 x 300 s of service) left out: 10 medium minutes."""
    plan = build_plan()
    plan.vehicles[0].node_indices = [0, 1]
    plan.rebuild_unassigned_pool()
    return plan


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit C++ default weights."""
    explicit = [type(spec)(weight=weight) for spec, (_, _, weight, _) in zip(get_constraints(), EXPECTED_INFO)]
    plan = _half_served_plan()
    raw = pdp.constraint_costs(plan)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == _engine(pdp.calculate_score(plan))


def test_the_medium_level_folds_into_soft():
    plan = _half_served_plan()
    raw = pdp.constraint_costs(plan)
    assert raw["unassigned_requests_cost"]["costs"] == [10.0]
    monolithic = pdp.calculate_score(plan)
    assert (monolithic.hard, monolithic.medium) == (0, -10)
    assert score(raw) == (0, monolithic.soft - 10 * MEDIUM_UNIT)
    others = [spec for spec in get_constraints() if spec.name != "unassigned_requests_cost"]
    assert score(raw, [*others, UnassignedRequestsCost(level="hard")]) == (
        -10,
        monolithic.soft,
    )
    assert score(raw, [*others, UnassignedRequestsCost(level="soft")]) == (
        0,
        monolithic.soft - 10,
    )
    assert score(raw, [*others, UnassignedRequestsCost(weight=3)]) == (
        0,
        monolithic.soft - 30 * MEDIUM_UNIT,
    )


def test_a_demoted_medium_constraint_is_soft_in_the_engine_too():
    """level="soft" moves unassigned_requests_cost to soft in the solver.

    With driving hard, parking every vehicle wins; the dropped requests then cost
    soft minutes, not medium ones."""
    picks = [
        paired_request_cost,
        same_vehicle_cost,
        UnassignedRequestsCost(level="soft"),
        TravelTimeCost(level="hard"),
    ]
    plan = solve(picks)
    assert (plan.score.hard, plan.score.medium) == (0, 0)
    assert plan.score.soft == -20  # both requests dropped, 10 minutes each
    assert score(pdp.constraint_costs(plan), picks) == (
        plan.score.hard,
        plan.score.soft,
    )


def _solve_reporting(run):
    """Solve the tiny plan with `run(solver, plan)`, recording every on_new_best score."""
    reported = []
    plan = build_plan()
    cfg = pdp.SolverConfig()
    cfg.spent_limit_seconds = 0.4
    cfg.random_seed = 1
    cfg.on_new_best = lambda _step, best: reported.append((best.hard, best.medium, best.soft))
    run(pdp.Solver(cfg), plan)
    return plan, reported


def test_the_plugin_solve_reports_each_level_unfolded():
    """plan.score and on_new_best on the plugin path read the calculator's levels, not a decoded fold.

    With unassigned_requests_cost demoted to soft, nothing is medium, so a soft
    total of 10^9 or more must stay soft in the final score and in every new best."""
    weight = 10_000_000
    picks = [
        vehicle_capacity_cost,
        paired_request_cost,
        same_vehicle_cost,
        pickup_before_dropoff_cost,
        UnassignedRequestsCost(level="soft", weight=MEDIUM_UNIT),
        TravelTimeCost(weight=weight),
    ]
    plan, reported = _solve_reporting(lambda solver, plan: solver.solve_with_constraints(plan, picks))
    raw = pdp.constraint_costs(plan)
    travel = sum(raw["travel_time_cost"]["costs"])
    unrouted = sum(raw["unassigned_requests_cost"]["costs"])
    final = (plan.score.hard, plan.score.medium, plan.score.soft)
    assert final == (0, 0, -(weight * travel + MEDIUM_UNIT * unrouted))
    assert -plan.score.soft >= MEDIUM_UNIT  # the case a decoded fold would misfile
    assert score(raw, picks) == _engine(plan.score)
    assert reported and all(medium == 0 for _, medium, _ in reported)
    assert reported[-1] == final


@pytest.mark.parametrize(
    "run",
    [
        pytest.param(lambda solver, plan: solver.solve(plan), id="monolithic"),
        pytest.param(
            lambda solver, plan: solver.solve_with_constraints(plan, get_constraints()),
            id="plugin",
        ),
    ],
)
def test_on_new_best_ends_on_the_final_score(run):
    """Each path reports strictly improving new bests in the levels of its final plan.score."""
    plan, reported = _solve_reporting(run)
    assert reported[-1] == (plan.score.hard, plan.score.medium, plan.score.soft)
    folded = [(hard, medium * MEDIUM_UNIT + soft) for hard, medium, soft in reported]
    assert all(earlier < later for earlier, later in zip(folded, folded[1:]))


def test_squared_order_age_is_opt_in():
    """Weight 0 by default: listed and measured, but it moves no score until weighted."""
    plan = _half_served_plan()
    raw = pdp.constraint_costs(plan)
    assert sum(raw["squared_order_age_cost"]["costs"]) > 0
    assert score(raw, [specs.squared_order_age_cost]) == (0, 0.0)
    assert score(raw, [SquaredOrderAgeCost(weight=1)]).soft == -sum(raw["squared_order_age_cost"]["costs"])


def test_on_new_best_with_two_workers_neither_hangs_nor_aborts():
    """Solver.solve releases the GIL, so two workers can call back (and raise) into Python."""
    cfg = pdp.SolverConfig()
    cfg.spent_limit_seconds = 0.3
    cfg.parallel_solver_count = 2
    calls = []
    cfg.on_new_best = lambda _step, best: calls.append(best)
    pdp.Solver(cfg).solve(build_plan())
    assert calls  # returned (a held GIL deadlocked here)

    def boom(_step, _best):
        raise RuntimeError("boom")

    cfg.on_new_best = boom
    with pytest.raises(RuntimeError, match="boom"):
        pdp.Solver(cfg).solve(build_plan())


def test_a_raising_on_new_best_stops_every_parallel_worker():
    """Four workers and a 30 s budget: the callback's exception (worker 0) stops the other
    workers too, so it reaches the caller at once instead of after their whole budget."""
    import time

    def boom(_step, _score):
        raise RuntimeError("boom")

    cfg = pdp.SolverConfig()
    cfg.spent_limit_seconds = 30.0
    cfg.parallel_solver_count = 4
    cfg.on_new_best = boom
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        pdp.Solver(cfg).solve(build_plan())
    assert time.monotonic() - started < 10.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path would call std::terminate."""
    import time

    from optimaru.pickup_delivery import _core

    cfg = pdp.SolverConfig()
    cfg.spent_limit_seconds = 30.0
    cfg.parallel_solver_count = 4
    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            pdp.Solver(cfg).solve(build_plan())
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


# --- repeated picks and the offset / clip knobs ------------------------------------

# Everything on a line, the position in seconds of driving from the depots.
HAND_PLACES = {"v": 0, "w": 0, "x": 0, "p1": 100, "d1": 200, "p2": 300, "d2": 400, "p3": 900, "d3": 1000}


def _hand_input():
    """Vehicle v (capacity 2) on r1, w on r2, x idle; r3 (2 x 120 s of service) in the pool.

    Raw per vehicle (v, w, x; the pool's cell is 0 for every route constraint):
    travel [200, 400, 0], capacity [2, 0, 0] (4 picked up on capacity 2);
    unassigned total 4 minutes.
    """
    order = list(HAND_PLACES)
    routes = {"v": ["p1", "d1"], "w": ["p2", "d2"], "x": []}
    service = {"r3": 120}

    def stop(name, request):
        return pdp.models.StopModel(id=name, location=(37.5, 127.0), service_duration=service.get(request, 0))

    return pdp.models.PdpInput(
        vehicles=[
            pdp.models.VehicleModel(
                id=name, capacity=2 if name == "v" else 10, home=(37.5, 127.0), max_stops=0, initial_route=route
            )
            for name, route in routes.items()
        ],
        deliveries=[
            pdp.models.DeliveryModel(
                id=f"r{i}", demand=4, pickup=stop(f"p{i}", f"r{i}"), dropoff=stop(f"d{i}", f"r{i}")
            )
            for i in (1, 2, 3)
        ],
        driving_seconds=[[abs(HAND_PLACES[p] - HAND_PLACES[q]) for q in order] for p in order],
    )


def _hand_plan():
    """The hand instance (`_hand_input`) as a plan."""
    return pdp.models.to_plan(_hand_input())


def _zero_budget(picks):
    """The engine's score of the hand plan under `picks`: a solve with no time (and no pair
    construction, which would route r3) keeps it as it is."""
    plan = _hand_plan()
    cfg = pdp.SolverConfig()
    cfg.spent_limit_seconds = 0
    cfg.random_seed = 1
    cfg.pair_construction_heuristic = False
    pdp.Solver(cfg).solve_with_constraints(plan, picks)
    assert [list(vehicle.node_indices) for vehicle in plan.vehicles] == [[0, 1], [2, 3], []]
    assert sorted(plan.unassigned_node_indices) == [4, 5]
    return _engine(plan.score)


REPEAT = [TravelTimeCost(), TravelTimeCost(weight=0.5, exponent=2)]


def test_a_repeated_pick_by_hand():
    """travel plus 0.5 x travel^2 per vehicle: 600 + 0.5 x (200^2 + 400^2) = 100600."""
    raw = pdp.constraint_costs(_hand_plan(), REPEAT)
    assert list(raw) == ["travel_time_cost", "travel_time_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [200.0, 400.0, 0.0, 0.0]
    assert first["units"] == second["units"] == [0, 1, 2]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 0.5, 2.0)
    assert first["name"] == second["name"] == "travel_time_cost"
    expected = (0, -(600 + 0.5 * (200**2 + 400**2)))
    assert expected == (0, -100600.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(pdp.constraint_costs(_hand_plan()), REPEAT) == expected
    assert _zero_budget(REPEAT) == expected


def test_identical_picks_count_twice():
    picks = [TravelTimeCost(), TravelTimeCost()]
    assert score(pdp.constraint_costs(_hand_plan()), picks) == (0, -1200.0)
    assert _zero_budget(picks) == (0, -1200.0)
    assert list(pdp.constraint_costs(_hand_plan(), picks)) == ["travel_time_cost", "travel_time_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        TravelTimeCost(weight=1),
        VehicleCapacityCost(),
        TravelTimeCost(weight=2),
        UnassignedRequestsCost(),
        TravelTimeCost(weight=3),
    ]
    raw = pdp.constraint_costs(_hand_plan(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "vehicle_capacity_cost": 1.0,
        "travel_time_cost": 1.0,
        "travel_time_cost#2": 2.0,
        "travel_time_cost#3": 3.0,
        "unassigned_requests_cost": 1.0,
    }
    assert [key for key in raw if key.startswith("travel")] == [
        "travel_time_cost",
        "travel_time_cost#2",
        "travel_time_cost#3",
    ]
    expected = (-2, -4 * MEDIUM_UNIT - 6 * 600)
    assert score(raw, picks) == expected
    assert _zero_budget(picks) == expected


def test_a_repeated_medium_pick_adds_up():
    picks = [UnassignedRequestsCost(), UnassignedRequestsCost(weight=2)]
    assert score(pdp.constraint_costs(_hand_plan()), picks) == (0, -12 * MEDIUM_UNIT)
    assert _zero_budget(picks) == (0, -12 * MEDIUM_UNIT)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Per vehicle; x is idle (raw 0) and the pool is no vehicle.
        (TravelTimeCost(offset=300), (0, -100.0)),
        (TravelTimeCost(offset=300, clip=False), (0, 300.0)),  # -100 + 100 - 300: x is rewarded too
        (TravelTimeCost(offset=500), (0, 0.0)),
        (TravelTimeCost(offset=-50), (0, -700.0)),  # a negative offset adds to every non-zero vehicle
        (TravelTimeCost(offset=-50, clip=False), (0, -750.0)),  # ...or to every vehicle
        (TravelTimeCost(offset=300, exponent=2), (0, -(100.0**2))),
        (VehicleCapacityCost(offset=1), (-1, 0.0)),
        (VehicleCapacityCost(offset=-0.5), (-3, 0.0)),  # hard rounds per vehicle: 2.5 -> 3, the zeros stay 0
        # The literal formula with an offset is refused on hard (the default of the
        # capacity, stop and coupling rules), so these picks sit on soft, where it charges
        # every evaluated unit: 2 x ((2 - 2.5) + (0 - 2.5) x 2) = -11 for the capacity.
        (VehicleCapacityCost(level="soft", offset=2.5, clip=False, weight=2), (0, 11.0)),
        (MaxStopsCost(level="soft", offset=2, clip=False), (0, 6.0)),  # all zero: 3 vehicles x -2, never the pool
        # required_vehicle_cost prices the pool: all four units are evaluated.
        (RequiredVehicleCost(level="soft", weight=1, offset=2, clip=False), (0, 8.0)),
        # The globals' totals are shaped once.
        (UnassignedRequestsCost(offset=2, exponent=2), (0, -4 * MEDIUM_UNIT)),
        (UnassignedRequestsCost(offset=6, clip=False), (0, 2 * MEDIUM_UNIT)),  # medium folds into soft
        (UnassignedRequestsCost(offset=6, clip=False, level="soft"), (0, 2.0)),
        (UnassignedRequestsCost(offset=6, level="hard"), (0, 0.0)),  # the threshold on hard: nothing beyond 6
        (PairedRequestCost(level="soft", offset=1, clip=False), (0, 1000.0)),  # 0 broken pairs, 1 below the offset
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(pdp.constraint_costs(_hand_plan()), [pick]) == expected
    raw = pdp.constraint_costs(_hand_plan(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert score(raw, [pick]) == expected
    assert _zero_budget([pick]) == expected


def test_the_route_constraints_evaluate_the_vehicles_and_not_the_pool():
    """Every per-vehicle entry lists the vehicles as its "units"; the two constraints that
    price the pool (and the globals) evaluate every unit and carry none."""
    raw = pdp.constraint_costs(_hand_plan())
    for name, entry in raw.items():
        if entry["global"] or name in ("required_vehicle_cost", "squared_order_age_cost"):
            assert "units" not in entry, name
        else:
            assert entry["units"] == [0, 1, 2], name
        if not entry["global"]:
            assert len(entry["costs"]) == 4, name
    assert raw["squared_order_age_cost"]["costs"][3] > 0  # the pool's dropoff d3 counts there


def test_offset_zero_reproduces_the_plain_scores():
    """offset 0 is the plain shaping in both modes, so a selection scores the same with or without it."""
    plain = [VehicleCapacityCost(), UnassignedRequestsCost(), TravelTimeCost(exponent=2)]
    shifted = [
        VehicleCapacityCost(offset=0.0, clip=False),
        UnassignedRequestsCost(offset=0),
        TravelTimeCost(exponent=2, offset=0.0, clip=False),
    ]
    raw = pdp.constraint_costs(_hand_plan())
    assert score(raw, shifted) == score(raw, plain) == (-2, -4 * MEDIUM_UNIT - (200**2 + 400**2))
    assert _zero_budget(shifted) == _zero_budget(plain) == score(raw, plain)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


@pytest.mark.parametrize(
    "picks",
    [
        [
            VehicleCapacityCost(),
            PairedRequestCost(),
            SameVehicleCost(),
            UnassignedRequestsCost(),
            TravelTimeCost(),
            TravelTimeCost(exponent=2, offset=1000),
        ],
        [
            VehicleCapacityCost(),
            VehicleCapacityCost(level="soft", offset=1, clip=False, weight=2),
            VehicleCapacityCost(offset=1, weight=2),
            PairedRequestCost(),
            PairedRequestCost(level="soft", offset=1, clip=False),
            PairedRequestCost(offset=1),
            MaxEndTimeCost(),
            UnassignedRequestsCost(offset=5),
            UnassignedRequestsCost(weight=2, offset=20, clip=False),
            TravelTimeCost(offset=-100),
            LatencyCost(weight=1, offset=3000, clip=False),
        ],
    ],
    ids=["repeat", "repeat-and-offsets"],
)
def test_score_matches_the_engine_for_repeated_and_shifted_picks(picks):
    """A real solve under repeated, shifted picks: the Python re-pricing of its costs is its score."""
    plan = solve(picks)
    raw = pdp.constraint_costs(plan, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python == _engine(plan.score)
    assert score(pdp.constraint_costs(plan), picks) == python  # the default costs score the repeats too


def test_naming_a_constraint_twice_scores_both_picks():
    """A constraint may be picked several times: each pick is scored on its own."""
    assert get_constraints([TravelTimeCost(), TravelTimeCost()]) == [TravelTimeCost(), TravelTimeCost()]
    picks = [VehicleCapacityCost(), TravelTimeCost(weight=10), TravelTimeCost()]
    assert _zero_budget(picks) == (-2, -(10 + 1) * 600.0)


# --- Levels: each pick, a repeat included, on the level it names --------------------


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        ([TravelTimeCost(level="medium")], (0, -600 * MEDIUM_UNIT)),
        ([TravelTimeCost(level=Level.HARD)], (-600, 0.0)),
        ([VehicleCapacityCost(level=Level.MEDIUM, weight=3)], (0, -6 * MEDIUM_UNIT)),  # weight in medium units
        ([VehicleCapacityCost(level="soft")], (0, -2.0)),
        ([UnassignedRequestsCost(level=Level.SOFT, weight=2)], (0, -8.0)),
        ([UnassignedRequestsCost(level="hard", offset=1)], (-3, 0.0)),
        # A repeat carries its own level and shaping: soft travel, medium beyond 300 s,
        # hard 2 x max(0, x - 300) per vehicle, and a literal copy 2 x (x - 300) per
        # vehicle, 2 x (-100 + 100 - 300) = -600, which may sit on soft but not on hard.
        (
            [
                TravelTimeCost(),
                TravelTimeCost(level="medium", offset=300),
                TravelTimeCost(level="hard", weight=2, offset=300),
                TravelTimeCost(level="soft", weight=2, offset=300, clip=False),
            ],
            (-200, -600 + 600 - 100 * MEDIUM_UNIT),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    assert score(pdp.constraint_costs(_hand_plan()), picks) == expected
    raw = pdp.constraint_costs(_hand_plan(), picks)
    for pick, key in zip(picks, instance_keys(picks)):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
    assert score(raw, picks) == expected
    assert _zero_budget(picks) == expected


def test_a_repeat_is_checked_on_its_own_level():
    """The second pick's level is read and checked like the first's, on both paths."""
    pick = SimpleNamespace(name="travel_time_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="constraint 'travel_time_cost': level must be 'hard', 'medium' or 'soft'"):
        _zero_budget([TravelTimeCost(), pick])
    with pytest.raises(ValueError, match="constraint 'travel_time_cost': level must be 'hard', 'medium' or 'soft'"):
        score(pdp.constraint_costs(_hand_plan()), [TravelTimeCost(), pick])


# --- clip=False with an offset stays off the hard score -------------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


def _refusal(call):
    with pytest.raises(ValueError) as refused:
        call()
    return str(refused.value)


@pytest.mark.parametrize("offset", [2.5, -2.5])
@pytest.mark.parametrize(
    "pick",
    [
        lambda offset: VehicleCapacityCost(offset=offset, clip=False),  # the default level, hard
        lambda offset: VehicleCapacityCost(level="hard", offset=offset, clip=False),
        lambda offset: RequiredVehicleCost(offset=offset, clip=False),  # hard, and it evaluates the pool
        lambda offset: PairedRequestCost(offset=offset, clip=False),  # a coupling global, hard
        lambda offset: TravelTimeCost(level="hard", offset=offset, clip=False),
        lambda offset: UnassignedRequestsCost(level=Level.HARD, offset=offset, clip=False),
    ],
    ids=[
        "capacity-default-hard",
        "capacity-level-hard",
        "required-vehicle-default-hard",
        "paired-default-hard",
        "travel-moved-to-hard",
        "unassigned-moved-to-hard",
    ],
)
def test_the_literal_offset_is_refused_on_hard_on_every_path(pick, offset):
    """A positive offset would reward the vehicles below it, so one vehicle's spare capacity
    would cancel another's overload and an infeasible plan could score hard 0; a negative
    one would charge every vehicle, an idle one included. The engine (solve_with_constraints,
    solve_request, constraint_costs) and the score() mirror refuse it with the same message;
    the CP-SAT reference's test_solve.py checks the same words."""
    spec = pick(offset)
    message = HARD_REFUSAL.format(q=repr(spec.name))
    repeated = [type(spec)(), spec]  # a refused repeat next to its accepted first instance
    assert _refusal(lambda: solve([spec], seconds=0.1)) == message
    assert _refusal(lambda: _zero_budget(repeated)) == message
    request = pdp.models.PdpRequest(model_input=_hand_input(), timelimit=0.1)
    assert _refusal(lambda: pdp.models.solve_request(request, constraints=[spec])) == message
    assert _refusal(lambda: pdp.constraint_costs(_hand_plan(), [spec])) == message
    assert _refusal(lambda: pdp.constraint_costs(_hand_plan(), repeated)) == message
    assert _refusal(lambda: score(pdp.constraint_costs(_hand_plan()), [spec])) == message
    assert _refusal(lambda: score(pdp.constraint_costs(_hand_plan()), repeated)) == message


@pytest.mark.parametrize("offset", [3.0, -3.0])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    """(2 - offset) + (0 - offset) x 2 per vehicle, on soft, on every path; never the pool."""
    pick = VehicleCapacityCost(level="soft", offset=offset, clip=False)
    expected = (0, -(2 - offset + 2 * (0 - offset)))
    assert score(pdp.constraint_costs(_hand_plan()), [pick]) == expected
    raw = pdp.constraint_costs(_hand_plan(), [pick])
    assert (raw["vehicle_capacity_cost"]["level"], raw["vehicle_capacity_cost"]["clip"]) == ("soft", False)
    assert score(raw, [pick]) == expected
    assert _zero_budget([pick]) == expected
    # Medium folds into soft here, so it takes the literal formula too.
    medium = VehicleCapacityCost(level="medium", offset=offset, clip=False)
    assert score(pdp.constraint_costs(_hand_plan()), [medium]) == (0, expected[1] * MEDIUM_UNIT)
    assert _zero_budget([medium]) == (0, expected[1] * MEDIUM_UNIT)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        (VehicleCapacityCost(offset=3.0), (0, 0.0)),  # clip=True on hard: a threshold
        (VehicleCapacityCost(offset=-3.0), (-5, 0.0)),  # 2 + 3 on the overloaded vehicle only
        (VehicleCapacityCost(clip=False), (-2, 0.0)),  # clip=False with offset 0: the plain shaping
        (VehicleCapacityCost(offset=0.0, clip=False), (-2, 0.0)),
        (PairedRequestCost(offset=-1), (0, 0.0)),  # no broken pair: a threshold adds nothing to 0
    ],
)
def test_the_allowed_combinations_on_hard(pick, expected):
    assert score(pdp.constraint_costs(_hand_plan()), [pick]) == expected
    assert _zero_budget([pick]) == expected


def test_the_refusal_comes_after_every_knob_of_a_duck_typed_pick():
    """The engine checks once a pick's knobs are all re-stated: the level a pick moves to decides."""
    moved = SimpleNamespace(
        name="vehicle_capacity_cost", weight=None, level="soft", exponent=1.0, offset=3.0, clip=False
    )
    assert _zero_budget([moved]) == (0, 7.0)
    kept = SimpleNamespace(name="vehicle_capacity_cost", weight=None, level=None, exponent=1.0, offset=3.0, clip=False)
    message = HARD_REFUSAL.format(q="'vehicle_capacity_cost'")
    assert _refusal(lambda: _zero_budget([kept])) == message
    assert _refusal(lambda: score(pdp.constraint_costs(_hand_plan()), [kept])) == message


# --- Negative thresholds: a fixed charge per non-zero vehicle, computed exactly -----


def _replacing(base, *picks):
    """`base` with each name's first pick replaced by the pick of that name in `picks`.

    Replacing, not appending: a second pick of a name is a second instance."""
    by_name = {pick.name: pick for pick in picks}
    chosen = [by_name.pop(spec.name, spec) for spec in base]
    return chosen + list(by_name.values())


# Whole seconds and integral weights: every contribution is a whole number, so the
# Python mirror reproduces the engine's levels exactly.
WHOLE_SECONDS = _replacing(
    get_constraints(),
    VehicleCapacityCost(offset=-0.5),  # hard: 0.5 extra per overloaded vehicle, rounded per vehicle
    UnassignedRequestsCost(offset=-1),
    TravelTimeCost(offset=-60),  # 60 s extra per vehicle that drives at all
    RouteDurationCost(weight=1, offset=-60),
    LatencyCost(weight=1, offset=-300),
) + [TravelTimeCost(exponent=2, offset=-30)]  # a second instance, appended on purpose
# Real-valued raw costs: the enclosing radius (meters) and the utilization shortfall.
REAL_VALUED = _replacing(
    WHOLE_SECONDS,
    EnclosingCircleRadiusCost(weight=0.01, offset=-60),
    MinCapacityUtilizationCost(weight=1000, offset=-0.05),
)


@pytest.mark.parametrize("picks", [WHOLE_SECONDS, REAL_VALUED], ids=["whole-seconds", "real-valued"])
def test_a_long_solve_with_negative_thresholds_scores_exactly(picks):
    """The threshold with an offset < 0 jumps at 0 (0 for an idle vehicle, |offset| ** e for
    any other), so a rounding residue on a raw cost that should be 0 would be charged in
    full. The engine computes those raw costs exactly: after a long seeded solve the last
    on_new_best report (the incremental score of the best solution) is plan.score (a fresh
    recompute), and the Python re-pricing of constraint_costs() is that score."""
    reports = []
    plan = pdp.build_random_instance(16, 4, seed=7)
    if picks is REAL_VALUED:
        vehicles = plan.vehicles
        for vehicle in vehicles:
            vehicle.min_load_ratio = 0.55
        plan.vehicles = vehicles
    cfg = pdp.SolverConfig()
    cfg.spent_limit_seconds = 60
    cfg.step_count_limit = 15_000
    cfg.random_seed = 5
    cfg.on_new_best = lambda _step, best: reports.append((best.hard, best.medium, best.soft))
    pdp.Solver(cfg).solve_with_constraints(plan, picks)
    final = (plan.score.hard, plan.score.medium, plan.score.soft)
    assert len(reports) > 1 and reports[-1] == final
    python = score(pdp.constraint_costs(plan, picks), picks)
    assert score(pdp.constraint_costs(plan), picks) == python  # the default costs say the same
    if picks is WHOLE_SECONDS:
        assert python == _engine(plan.score)
    else:
        # plan.score rounds its soft level to a whole number; the mirror keeps the fraction.
        assert python.hard == plan.score.hard
        assert abs(python.soft - _engine(plan.score)[1]) <= 0.5 + 1e-3

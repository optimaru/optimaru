"""pdp_routing.constraints — an API for picking the cost function declaratively.

"Which constraints are on", the catalog (`constraint_info()`), the three knobs and
the medium level are covered here. Per-constraint values are covered by
`test_pdp.py` and the C++ tests (one hand-calculated positive and zero case each).
"""

import pickle
import re
from types import SimpleNamespace

import pdp_routing as pdp
import pytest
from pdp_routing import constraints as specs
from pdp_routing.constraints import (
    Constraint,
    SquaredOrderAgeCost,
    TravelTimeCost,
    UnassignedRequestsCost,
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
    # Unit constraints first, global constraints (the three coupling terms + unassigned) after.
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
        "unassigned_requests_cost",
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

    without_travel = solve(
        [vehicle_capacity_cost, paired_request_cost, same_vehicle_cost]
    ).score
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

    hard_level = solve([*picks, TravelTimeCost(hard=True)]).score
    assert hard_level.medium < 0  # gives up requests instead
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "same_vehicle_costs"  # trailing s
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
    """Module-level instances must keep the defaults (weight=None, hard=None, exponent=1)."""
    for spec in get_constraints():
        assert (spec.weight, spec.hard, spec.exponent) == (None, None, 1.0)
        assert (
            getattr(specs, spec.name) is spec
        )  # the instance is named after the constraint
    # The opt-in objectives no longer pass an explicit weight=0: the C++ default is 0.
    assert specs.enclosing_circle_radius_cost.weight is None


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


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


def test_score_rejects_an_unknown_constraint_name():
    """A spec with a wrong name (e.g. a renamed component) must not be silently ignored."""
    raw, _ = _raw()

    class Renamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Renamed()])


def order_age_input(order_times: list[int]) -> pdp.models.PdpInput:
    """One vehicle at 0, and requests whose stops are all at the same place, 100 s away."""
    size = 1 + 2 * len(order_times)
    seconds = [
        [0 if i == j or (i and j) else 100 for j in range(size)] for i in range(size)
    ]
    stop = lambda name: pdp.models.StopModel(
        id=name, location=(37.5, 127.0), max_end_time=10**6, service_duration=10
    )
    return pdp.models.PdpInput(
        vehicles=[
            pdp.models.VehicleModel(id="v", capacity=5, home=(37.5, 127.0), max_stops=0)
        ],
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
    together = order_age_cost(
        order_age_input([0, 0]), [0, 2, 1, 3]
    )  # finish at 130, 140
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
    constraints = [
        c for c in pdp.constraints.get_constraints() if c.name not in replaced
    ]
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
    ("unassigned_requests_cost", "medium", 1.0, True),
]
GLOBALS = {
    "paired_request_cost",
    "same_vehicle_cost",
    "pickup_before_dropoff_cost",
    "unassigned_requests_cost",
}


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = pdp.constraint_info()
    assert [row.name for row in rows] == pdp.list_constraints()
    assert [
        (row.name, row.default_level, row.default_weight, row.default_enabled)
        for row in rows
    ] == EXPECTED_INFO
    # Every constraint is flippable in the engine (the coupling rules are structural
    # on the CP-SAT path only) and none is a reward.
    assert all(row.level_flippable and not row.reward for row in rows)


def test_constraint_info_rows_are_plain_values():
    row = pdp.constraint_info()[-1]
    assert isinstance(row, pdp.ConstraintInfo)
    assert row._asdict() == {
        "name": "unassigned_requests_cost",
        "default_level": "medium",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='unassigned_requests_cost'")
    assert pickle.loads(pickle.dumps(pdp.constraint_info())) == pdp.constraint_info()
    assert pdp.ConstraintInfo.__module__ == pdp._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults and carries the domain's medium fold on every entry."""
    plan = build_plan()
    plan.rebuild_unassigned_pool()
    raw = pdp.constraint_costs(plan)
    assert list(raw) == pdp.list_constraints()
    for row in pdp.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
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
    """The engine-level (hard, soft) of a HardMediumSoft plan score (medium folded into soft)."""
    return score_.hard, score_.medium * MEDIUM_UNIT + score_.soft


def _half_served_plan():
    """r1 routed, r2 (2 x 300 s of service) left out: 10 medium minutes."""
    plan = build_plan()
    plan.vehicles[0].node_indices = [0, 1]
    plan.rebuild_unassigned_pool()
    return plan


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit C++ default weights."""
    explicit = [
        type(spec)(weight=weight)
        for spec, (_, _, weight, _) in zip(get_constraints(), EXPECTED_INFO)
    ]
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
    others = [
        spec for spec in get_constraints() if spec.name != "unassigned_requests_cost"
    ]
    assert score(raw, [*others, UnassignedRequestsCost(hard=True)]) == (
        -10,
        monolithic.soft,
    )
    assert score(raw, [*others, UnassignedRequestsCost(hard=False)]) == (
        0,
        monolithic.soft - 10,
    )
    assert score(raw, [*others, UnassignedRequestsCost(weight=3)]) == (
        0,
        monolithic.soft - 30 * MEDIUM_UNIT,
    )


def test_a_demoted_medium_constraint_is_soft_in_the_engine_too():
    """hard=False moves unassigned_requests_cost to soft in the solver (it was a no-op before 0.2.0).

    With driving hard, parking every vehicle wins; the dropped requests then cost
    soft minutes, not medium ones."""
    picks = [
        paired_request_cost,
        same_vehicle_cost,
        UnassignedRequestsCost(hard=False),
        TravelTimeCost(hard=True),
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
    cfg.on_new_best = lambda _step, best: reported.append(
        (best.hard, best.medium, best.soft)
    )
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
        UnassignedRequestsCost(hard=False, weight=MEDIUM_UNIT),
        TravelTimeCost(weight=weight),
    ]
    plan, reported = _solve_reporting(
        lambda solver, plan: solver.solve_with_constraints(plan, picks)
    )
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


@pytest.mark.parametrize(
    ("kwargs", "error"),
    [
        ({"weight": -1}, ValueError),
        ({"weight": float("nan")}, ValueError),
        ({"weight": "2"}, TypeError),
        ({"exponent": 0}, ValueError),
        ({"hard": 1}, TypeError),
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        TravelTimeCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""
    negative = SimpleNamespace(
        name="travel_time_cost", weight=-2.0, hard=None, exponent=1.0
    )
    flat = SimpleNamespace(
        name="travel_time_cost", weight=None, hard=None, exponent=0.0
    )
    for pick, message in (
        (negative, "weight must be finite and >= 0"),
        (flat, "exponent must be finite and > 0"),
    ):
        with pytest.raises(ValueError, match=message):
            solve([pick])


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"hard": 1}, "hard must be True, False or None, not 1"),
        ({"hard": "x"}, "hard must be True, False or None, not 'x'"),
        ({"exponent": True}, "exponent must be a number, not True"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(
        name="unassigned_requests_cost", weight=None, hard=None, exponent=1.0
    )
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(
        TypeError, match=f"constraint 'unassigned_requests_cost': {re.escape(message)}"
    ):
        solve([pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        UnassignedRequestsCost(**knobs)


def test_squared_order_age_is_opt_in():
    """Weight 0 by default: listed and measured, but it moves no score until weighted."""
    plan = _half_served_plan()
    raw = pdp.constraint_costs(plan)
    assert sum(raw["squared_order_age_cost"]["costs"]) > 0
    assert score(raw, [specs.squared_order_age_cost]) == (0, 0.0)
    assert score(raw, [SquaredOrderAgeCost(weight=1)]).soft == -sum(
        raw["squared_order_age_cost"]["costs"]
    )


# 0.2.0 migration: the pre-0.2.0 catalog names (vehicle_fixed_cost kept its name).
OLD_NAMES = [
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
    "minimize_lateness",
    "pinned_stops",
    "required_vehicle",
    "paired_request",
    "same_vehicle",
    "pickup_before_dropoff",
    "unassigned_requests",
]


@pytest.mark.parametrize("old", OLD_NAMES)
def test_old_names_are_gone(old):
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    pick = SimpleNamespace(name=old, weight=None, hard=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        solve([pick], seconds=0.05)
    plan = build_plan()
    plan.rebuild_unassigned_pool()
    with pytest.raises(ValueError, match="unknown constraint"):
        score(pdp.constraint_costs(plan), [pick])


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
    at once. A joinable std::thread destroyed on that path used to call std::terminate."""
    import time

    from pdp_routing import _core

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

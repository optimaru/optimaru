"""vrp_routing.constraints — an API for picking the cost function declaratively.

"Which constraints are on", the catalog (`constraint_info()`), the five knobs, repeated
picks and the medium level are covered here. Per-constraint values are covered by
`test_routing.py` and the C++ tests (one hand-calculated positive and zero case each).
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import vrp_routing as vrp
from vrp_routing import constraints as specs
from vrp_routing.constraints import (
    Constraint,
    EnclosingCircleRadiusCost,
    LatencyCost,
    MaxEndTimeCost,
    MaxStopsCost,
    RouteDurationCost,
    TravelTimeCost,
    UnassignedVisitsCost,
    VehicleCapacityCost,
    get_constraints,
    score,
    travel_time_cost,
    unassigned_visits_cost,
    vehicle_capacity_cost,
)
from vrp_routing.models import RoutingInput, VehicleModel, VisitModel, to_plan

MEDIUM_UNIT = 1_000_000_000  # the domain's medium fold (into soft)


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
    """Names carried by specs must be names the core actually knows."""
    # Unit constraints first, the global constraint (unassigned_visits_cost) after.
    available = vrp.list_constraints()
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
        "vehicle_fixed_cost",
        "unassigned_visits_cost",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == vrp.list_constraints()


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


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
    assert costs["max_stops_cost"]["costs"][0] == 1
    assert costs["max_travel_time_cost"]["costs"][0] > 0
    assert costs["max_route_duration_cost"]["costs"][0] > 0
    assert costs["vehicle_max_end_time_cost"]["costs"][0] > 0
    assert costs["vehicle_fixed_cost"]["costs"][0] == 25
    assert costs["max_route_radius_cost"]["costs"][0] > 0


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """A cost function assembled from all default specs must equal the monolithic scoring."""
    plan = solve(get_constraints())
    recomputed = vrp.calculate_score(plan)
    assert plan.score.hard == recomputed.hard
    assert plan.score.medium == recomputed.medium
    assert plan.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the travel-time term disappears entirely."""
    baseline = solve(get_constraints()).score
    assert baseline.soft < 0

    without_travel = solve([vehicle_capacity_cost, unassigned_visits_cost]).score
    assert without_travel.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the travel-time weight grows soft only; hard stays put."""
    picks = [vehicle_capacity_cost, unassigned_visits_cost]
    base = solve([*picks, travel_time_cost]).score
    heavier = solve([*picks, TravelTimeCost(weight=10)]).score
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Raising the level flips the solver's trade-off.

    With soft travel time, every visit gets served and the cost is paid in soft.
    Promoted to hard, driving itself becomes a hard cost while unassigned stays
    medium — levels are lexicographic — so the solver parks the vehicles."""
    picks = [unassigned_visits_cost]

    soft_level = solve([*picks, travel_time_cost]).score
    assert soft_level.medium == 0  # serves every visit
    assert soft_level.soft < 0

    hard_level = solve([*picks, TravelTimeCost(hard=True)]).score
    assert hard_level.medium < 0  # gives up visits instead
    assert hard_level.soft == 0
    assert hard_level.hard == 0  # nothing drives, so no hard cost either


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "travel_time_costs"  # trailing s
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


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    plan = solve(get_constraints())
    return vrp.constraint_costs(plan), plan


def test_score_reproduces_the_engine_hard_level():
    """The hard level of the Python score at default specs must match the engine."""
    raw, solution = _raw()
    assert score(raw).hard == vrp.calculate_score(solution).hard


def test_score_is_additive_over_the_picked_constraints():
    """The total score is the sum of per-constraint scores — unlisted terms simply drop out."""
    raw, _ = _raw()
    picked = get_constraints()
    full = score(raw, picked)
    parts = [score(raw, [spec]) for spec in picked]
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


# --- The catalog: constraint_info() ------------------------------------------

# (name, default_level, default_weight, default_enabled), list_constraints() order.
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
    ("vehicle_fixed_cost", "soft", 1.0, True),
    ("unassigned_visits_cost", "medium", 1.0, True),
]


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = vrp.constraint_info()
    assert [row.name for row in rows] == vrp.list_constraints()
    assert [
        (row.name, row.default_level, row.default_weight, row.default_enabled)
        for row in rows
    ] == EXPECTED_INFO
    # Every constraint is flippable and none is a reward.
    assert all(row.level_flippable and not row.reward for row in rows)


def test_constraint_info_rows_are_plain_values():
    row = vrp.constraint_info()[-1]
    assert isinstance(row, vrp.ConstraintInfo)
    assert row._asdict() == {
        "name": "unassigned_visits_cost",
        "default_level": "medium",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='unassigned_visits_cost'")
    assert pickle.loads(pickle.dumps(vrp.constraint_info())) == vrp.constraint_info()
    assert vrp.ConstraintInfo.__module__ == vrp._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults and carries the domain's medium fold on every entry."""
    plan = small_plan()
    plan.rebuild_unassigned_pool()
    raw = vrp.constraint_costs(plan)
    assert list(raw) == vrp.list_constraints()
    for row in vrp.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"], entry["global"]) == (
            True,
            False,
            row.name == "unassigned_visits_cost",
        )
        assert (entry["name"], entry["label"]) == (row.name, "")
        assert entry["medium_fold"] == {"kind": "into_soft", "unit": float(MEDIUM_UNIT)}


# --- The knobs ------------------------------------------------------------------


def _engine(score_):
    """The engine-level (hard, soft) of a HardMediumSoft plan score (medium folded into soft)."""
    return score_.hard, score_.medium * MEDIUM_UNIT + score_.soft


def _half_served_plan():
    """V1 serves C0-C2 (demand 6 = capacity); C3-C5 (600 s of service each) are left out: 30 medium minutes."""
    plan = small_plan()
    plan.vehicles[0].visit_indices = [0, 1, 2]
    plan.rebuild_unassigned_pool()
    return plan


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit C++ default weights."""
    explicit = [
        type(spec)(weight=weight)
        for spec, (_, _, weight, _) in zip(get_constraints(), EXPECTED_INFO)
    ]
    plan = _half_served_plan()
    raw = vrp.constraint_costs(plan)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == _engine(vrp.calculate_score(plan))


def test_the_medium_level_folds_into_soft():
    plan = _half_served_plan()
    raw = vrp.constraint_costs(plan)
    assert raw["unassigned_visits_cost"]["costs"] == [30.0]
    monolithic = vrp.calculate_score(plan)
    assert (monolithic.hard, monolithic.medium) == (0, -30)
    assert score(raw) == (0, monolithic.soft - 30 * MEDIUM_UNIT)
    others = [
        spec for spec in get_constraints() if spec.name != "unassigned_visits_cost"
    ]
    assert score(raw, [*others, UnassignedVisitsCost(hard=True)]) == (
        -30,
        monolithic.soft,
    )
    assert score(raw, [*others, UnassignedVisitsCost(hard=False)]) == (
        0,
        monolithic.soft - 30,
    )
    assert score(raw, [*others, UnassignedVisitsCost(weight=3)]) == (
        0,
        monolithic.soft - 90 * MEDIUM_UNIT,
    )


def test_a_demoted_medium_constraint_is_soft_in_the_engine_too():
    """hard=False moves unassigned_visits_cost to soft in the solver (it was a no-op before 0.2.0).

    With driving hard, parking every vehicle wins; the dropped visits then cost
    soft minutes, not medium ones."""
    picks = [
        vehicle_capacity_cost,
        UnassignedVisitsCost(hard=False),
        TravelTimeCost(hard=True),
    ]
    plan = solve(picks)
    assert (plan.score.hard, plan.score.medium) == (0, 0)
    assert plan.score.soft == -60  # all six visits dropped, 10 minutes each
    assert score(vrp.constraint_costs(plan), picks) == (
        plan.score.hard,
        plan.score.soft,
    )


def test_the_plugin_solve_reports_each_level_unfolded():
    """plan.score on the plugin path reads the calculator's three levels, not a decoded fold.

    A soft total of 10^9 or more stays on the soft level instead of spilling into medium."""
    weight = 10_000_000
    picks = [
        vehicle_capacity_cost,
        unassigned_visits_cost,
        TravelTimeCost(weight=weight),
    ]
    plan = solve(picks)
    raw = vrp.constraint_costs(plan)
    travel = sum(raw["travel_time_cost"]["costs"])
    unrouted = sum(raw["unassigned_visits_cost"]["costs"])
    assert travel * weight >= MEDIUM_UNIT  # the case a decoded fold would misfile
    assert (plan.score.hard, plan.score.medium, plan.score.soft) == (
        0,
        -unrouted,
        -weight * travel,
    )
    assert score(raw, picks) == _engine(plan.score)


def test_on_new_best_reports_the_levels_of_the_final_score():
    """on_new_best on the plugin path reports the same three levels as plan.score.

    The last new best is the returned solution, so with a soft total of 10^9 or
    more its report still equals plan.score instead of a decoded fold."""
    weight = 10_000_000
    picks = [
        vehicle_capacity_cost,
        unassigned_visits_cost,
        TravelTimeCost(weight=weight),
    ]
    reports = []
    plan = small_plan()
    cfg = vrp.SolverConfig()
    cfg.spent_limit_seconds = 0.4
    cfg.random_seed = 7
    cfg.on_new_best = lambda _step, best: reports.append(
        (best.hard, best.medium, best.soft)
    )
    vrp.Solver(cfg).solve_with_constraints(plan, picks)
    final = (plan.score.hard, plan.score.medium, plan.score.soft)
    assert final[2] <= -MEDIUM_UNIT
    assert reports and reports[-1] == final


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
    unbounded = SimpleNamespace(
        name="travel_time_cost", weight=None, hard=None, exponent=1.0, offset=float("inf")
    )
    for pick, message in (
        (negative, "weight must be finite and >= 0"),
        (flat, "exponent must be finite and > 0"),
        (unbounded, "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            solve([pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        TravelTimeCost(offset=float("nan"))


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"hard": 1}, "hard must be True, False or None, not 1"),
        ({"hard": "x"}, "hard must be True, False or None, not 'x'"),
        ({"exponent": True}, "exponent must be a number, not True"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(
        name="unassigned_visits_cost", weight=None, hard=None, exponent=1.0
    )
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(
        TypeError, match=f"constraint 'unassigned_visits_cost': {re.escape(message)}"
    ):
        solve([pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        UnassignedVisitsCost(**knobs)


@pytest.mark.parametrize(
    "spec_class", [EnclosingCircleRadiusCost, RouteDurationCost, LatencyCost]
)
def test_the_weight_zero_objectives_are_opt_in(spec_class):
    """Weight 0 by default: listed and measured, but they move no score until weighted."""
    plan = small_plan()
    plan.vehicles[0].visit_indices = [0, 1, 2]
    plan.vehicles[1].visit_indices = [3, 4, 5]
    plan.rebuild_unassigned_pool()
    raw = vrp.constraint_costs(plan)
    name = spec_class.name
    assert sum(raw[name]["costs"]) > 0
    assert score(raw, [getattr(specs, name)]) == (0, 0.0)
    assert score(raw, [spec_class(weight=1)]).soft == pytest.approx(
        -sum(raw[name]["costs"])
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
    "maximize_visits_assigned",
]


@pytest.mark.parametrize("old", OLD_NAMES)
def test_old_names_are_gone(old):
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    pick = SimpleNamespace(name=old, weight=None, hard=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        solve([pick], seconds=0.05)
    plan = small_plan()
    plan.rebuild_unassigned_pool()
    with pytest.raises(ValueError, match="unknown constraint"):
        score(vrp.constraint_costs(plan), [pick])


def test_on_new_best_with_two_workers_neither_hangs_nor_aborts():
    """Solver.solve releases the GIL, so two workers can call back (and raise) into Python."""
    cfg = vrp.SolverConfig()
    cfg.spent_limit_seconds = 0.3
    cfg.parallel_solver_count = 2
    calls = []
    cfg.on_new_best = lambda _step, best: calls.append(best)
    plan = small_plan()
    vrp.Solver(cfg).solve(plan)
    assert calls  # returned (a held GIL deadlocked here)

    def boom(_step, _best):
        raise RuntimeError("boom")

    cfg.on_new_best = boom
    with pytest.raises(RuntimeError, match="boom"):
        vrp.Solver(cfg).solve(small_plan())


def test_a_raising_on_new_best_stops_every_parallel_worker():
    """Four workers and a 30 s budget: the callback's exception (worker 0) stops the other
    workers too, so it reaches the caller at once instead of after their whole budget."""
    import time

    def boom(_step, _score):
        raise RuntimeError("boom")

    cfg = vrp.SolverConfig()
    cfg.spent_limit_seconds = 30.0
    cfg.parallel_solver_count = 4
    cfg.on_new_best = boom
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        vrp.Solver(cfg).solve(small_plan())
    assert time.monotonic() - started < 10.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path used to call std::terminate."""
    import time

    from vrp_routing import _core

    cfg = vrp.SolverConfig()
    cfg.spent_limit_seconds = 30.0
    cfg.parallel_solver_count = 4
    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            vrp.Solver(cfg).solve(small_plan())
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------

# Everything on a line, the position in seconds of driving from the depots.
HAND_PLACES = {"v": 0, "w": 0, "x": 0, "a": 100, "b": 300, "c": 600, "d": 900, "e": 1000}


def _hand_plan():
    """Vehicle v (capacity 1) on a, b; w on c; x idle; d and e in the pool (5 and 3 minutes).

    Raw per vehicle (v, w, x; the pool's cell is always 0): travel [300, 600, 0],
    capacity [1, 0, 0]; unassigned total 8 penalty minutes.
    """
    order = list(HAND_PLACES)
    routes = {"v": ["a", "b"], "w": ["c"], "x": []}
    model = RoutingInput(
        vehicles=[
            VehicleModel(id=name, capacity=1 if name == "v" else 5, depot=(37.5, 127.0), initial_route=route)
            for name, route in routes.items()
        ],
        visits=[
            VisitModel(id=name, location=(37.5, 127.0), unassigned_penalty_minutes={"d": 5, "e": 3}.get(name, -1))
            for name in "abcde"
        ],
        driving_seconds=[[abs(HAND_PLACES[p] - HAND_PLACES[q]) for q in order] for p in order],
    )
    plan = to_plan(model)
    plan.rebuild_unassigned_pool()
    return plan


def _zero_budget(picks):
    """The engine's score of the hand plan under `picks`: a solve with no time keeps it as it is."""
    plan = _hand_plan()
    cfg = vrp.SolverConfig()
    cfg.spent_limit_seconds = 0
    cfg.random_seed = 1
    vrp.Solver(cfg).solve_with_constraints(plan, picks)
    assert [list(vehicle.visit_indices) for vehicle in plan.vehicles] == [[0, 1], [2], []]
    assert list(plan.unassigned_visit_indices) == [3, 4]
    return _engine(plan.score)


REPEAT = [TravelTimeCost(), TravelTimeCost(weight=0.5, exponent=2)]


def test_a_repeated_pick_by_hand():
    """travel plus 0.5 x travel^2 per vehicle: 900 + 0.5 x (300^2 + 600^2) = 225900."""
    raw = vrp.constraint_costs(_hand_plan(), REPEAT)
    assert list(raw) == ["travel_time_cost", "travel_time_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [300.0, 600.0, 0.0, 0.0]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 0.5, 2.0)
    assert first["name"] == second["name"] == "travel_time_cost"
    assert second["default_weight"] == 1.0
    expected = (0, -(900 + 0.5 * (300**2 + 600**2)))
    assert expected == (0, -225900.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(vrp.constraint_costs(_hand_plan()), REPEAT) == expected
    assert _zero_budget(REPEAT) == expected


def test_identical_picks_count_twice():
    picks = [TravelTimeCost(), TravelTimeCost()]
    assert score(vrp.constraint_costs(_hand_plan()), picks) == (0, -1800.0)
    assert _zero_budget(picks) == (0, -1800.0)
    assert list(vrp.constraint_costs(_hand_plan(), picks)) == ["travel_time_cost", "travel_time_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        TravelTimeCost(weight=1),
        VehicleCapacityCost(),
        TravelTimeCost(weight=2),
        UnassignedVisitsCost(),
        TravelTimeCost(weight=3),
    ]
    raw = vrp.constraint_costs(_hand_plan(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "vehicle_capacity_cost": 1.0,
        "travel_time_cost": 1.0,
        "travel_time_cost#2": 2.0,
        "travel_time_cost#3": 3.0,
        "unassigned_visits_cost": 1.0,
    }
    assert [key for key in raw if key.startswith("travel")] == [
        "travel_time_cost",
        "travel_time_cost#2",
        "travel_time_cost#3",
    ]
    expected = (-1, -8 * MEDIUM_UNIT - 6 * 900)
    assert score(raw, picks) == expected
    assert _zero_budget(picks) == expected


def test_a_repeated_medium_pick_adds_up():
    picks = [UnassignedVisitsCost(), UnassignedVisitsCost(weight=2)]
    assert score(vrp.constraint_costs(_hand_plan()), picks) == (0, -24 * MEDIUM_UNIT)
    assert _zero_budget(picks) == (0, -24 * MEDIUM_UNIT)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Per vehicle; x is idle (raw 0) and the pool is no vehicle.
        (TravelTimeCost(offset=400), (0, -200.0)),
        (TravelTimeCost(offset=400, clip=False), (0, 300.0)),  # -100 + 200 - 400: x is rewarded too
        (TravelTimeCost(offset=700), (0, 0.0)),
        (TravelTimeCost(offset=-50), (0, -1000.0)),  # a negative offset adds to every non-zero vehicle
        (TravelTimeCost(offset=-50, clip=False), (0, -1050.0)),  # ...or to every vehicle
        (TravelTimeCost(offset=500, exponent=2), (0, -(100.0**2))),
        (TravelTimeCost(offset=400, exponent=3, clip=False), (0, -(-(100**3) + 200**3 - 400**3))),
        (VehicleCapacityCost(offset=3, clip=False), (8, 0.0)),  # (1 - 3) + (0 - 3) x 2: a hard reward below 3
        (VehicleCapacityCost(offset=1.5, clip=False), (5, 0.0)),  # -0.5 -> -1, -1.5 -> -2 twice: per vehicle
        # The global's total (8 minutes) is shaped once: (8 - 2)^2, not (5 - 2)^2 + (3 - 2)^2.
        (UnassignedVisitsCost(offset=2, exponent=2), (0, -36 * MEDIUM_UNIT)),
        (UnassignedVisitsCost(offset=10, clip=False), (0, 2 * MEDIUM_UNIT)),
        (UnassignedVisitsCost(offset=10, clip=False, hard=True), (2, 0.0)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(vrp.constraint_costs(_hand_plan()), [pick]) == expected
    raw = vrp.constraint_costs(_hand_plan(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert score(raw, [pick]) == expected
    assert _zero_budget([pick]) == expected


def test_the_route_constraints_evaluate_the_vehicles_and_not_the_pool():
    """Every per-vehicle entry lists the vehicles as its "units"; the literal offset charges
    exactly those (3 x 2 hard on an all-zero max_stops_cost), never the pool's cell."""
    raw = vrp.constraint_costs(_hand_plan())
    for name, entry in raw.items():
        if entry["global"]:
            assert "units" not in entry
        else:
            assert entry["units"] == [0, 1, 2], name
            assert len(entry["costs"]) == 4 and entry["costs"][3] == 0.0, name
    pick = MaxStopsCost(offset=2, clip=False)
    assert raw["max_stops_cost"]["costs"] == [0.0, 0.0, 0.0, 0.0]
    assert score(raw, [pick]) == (6, 0.0)
    assert _zero_budget([pick]) == (6, 0.0)


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [VehicleCapacityCost(), UnassignedVisitsCost(), TravelTimeCost(exponent=2)]
    shifted = [
        VehicleCapacityCost(offset=0.0, clip=False),
        UnassignedVisitsCost(offset=0),
        TravelTimeCost(exponent=2, offset=0.0, clip=False),
    ]
    raw = vrp.constraint_costs(_hand_plan())
    assert score(raw, shifted) == score(raw, plain) == (-1, -8 * MEDIUM_UNIT - (300**2 + 600**2))
    assert _zero_budget(shifted) == _zero_budget(plain) == score(raw, plain)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


@pytest.mark.parametrize(
    "picks",
    [
        [VehicleCapacityCost(), UnassignedVisitsCost(), TravelTimeCost(), TravelTimeCost(exponent=2, offset=1000)],
        [
            VehicleCapacityCost(),
            VehicleCapacityCost(offset=1, clip=False, weight=2),
            MaxEndTimeCost(),
            UnassignedVisitsCost(offset=5),
            UnassignedVisitsCost(weight=2, offset=20, clip=False),
            TravelTimeCost(offset=-100),
            LatencyCost(weight=1, offset=3000, clip=False),
        ],
    ],
    ids=["repeat", "repeat-and-offsets"],
)
def test_score_matches_the_engine_for_repeated_and_shifted_picks(picks):
    """A real solve under repeated, shifted picks: the Python re-pricing of its costs is its score."""
    plan = solve(picks)
    raw = vrp.constraint_costs(plan, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python == _engine(plan.score)
    assert score(vrp.constraint_costs(plan), picks) == python  # the default costs score the repeats too


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own."""
    assert get_constraints([TravelTimeCost(), TravelTimeCost()]) == [TravelTimeCost(), TravelTimeCost()]
    picks = [VehicleCapacityCost(), TravelTimeCost(weight=10), TravelTimeCost()]
    assert _zero_budget(picks) == (-1, -(10 + 1) * 900.0)

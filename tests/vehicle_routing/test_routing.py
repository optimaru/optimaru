"""Correctness tests for the optimaru.vehicle_routing package.

Requires the compiled extension: python3 setup.py build_ext --inplace
"""

import pytest

import optimaru.vehicle_routing as vrp
from optimaru.vehicle_routing import models
from optimaru.vehicle_routing.constraints import TravelDistanceCost, UnassignedVisitsCost, get_constraints

from .test_constraint_specs import _random_plan


def test_request_solver_parameters_are_applied():
    config = vrp.SolverConfig()
    models.apply_solver_parameters(config, {"late_acceptance_size": 7, "ruin_recreate": False})
    assert config.late_acceptance_size == 7
    assert config.ruin_recreate is False
    with pytest.raises(ValueError, match="unknown solver parameter"):
        models.apply_solver_parameters(config, {"not_a_parameter": 1})


def test_granular_descent_parameters_are_public():
    config = vrp.SolverConfig()
    assert config.granular_descent is True
    config.granular_descent = False
    config.granular_descent_max_segment = 3
    config.granular_descent_candidate_routes = 6
    assert not config.granular_descent
    assert config.granular_descent_max_segment == 3
    assert config.granular_descent_candidate_routes == 6


# Also the small problem of tests/_problems.py.
def small_plan():
    """2 vehicles / 6 visits around Seoul; capacities force a split."""
    locations = [
        vrp.Location(37.5665, 126.9780),  # 0: V1 depot
        vrp.Location(37.4979, 127.0276),  # 1: V2 depot
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
        visits.append(vrp.Visit(f"C{i}", f"Customer {i}", 2 + i, 2, 8 * 3600, 20 * 3600, 600))
    vehicles = [
        vrp.Vehicle("V1", 6, 0, departure_time=7 * 3600),
        vrp.Vehicle("V2", 6, 1, departure_time=7 * 3600),
    ]
    return vrp.VehicleRoutePlan("test", locations, vehicles, visits)


def solve(plan, seconds=0.3, seed=7):
    cfg = vrp.SolverConfig()
    cfg.spent_limit_seconds = seconds
    cfg.random_seed = seed
    vrp.Solver(cfg).solve(plan)
    return plan


def test_solve_assigns_everything_feasibly():
    plan = solve(small_plan())
    assert plan.score.is_feasible()
    assert plan.score.medium == 0  # no unassigned visits
    routed = [i for v in plan.vehicles for i in v.visit_indices]
    assert sorted(routed) == list(range(6))
    # Capacity respected per route (6 visits x demand 2 over capacity 6+6).
    for v in plan.vehicles:
        assert sum(plan.visits[i].demand for i in v.visit_indices) <= v.capacity


def test_score_matches_recompute_and_shadows_are_set():
    plan = solve(small_plan())
    final = str(plan.score)
    assert str(vrp.calculate_score(plan)) == final
    for v in plan.vehicles:
        for i in v.visit_indices:
            visit = plan.visits[i]
            assert visit.arrival_time >= 0
            assert visit.vehicle_index >= 0


def test_seeded_runs_are_reproducible():
    a = solve(small_plan(), seed=3)
    b = solve(small_plan(), seed=3)
    assert str(a.score) == str(b.score)
    assert a.routes() == b.routes()


def test_pydantic_models_round_trip():
    from optimaru.vehicle_routing import models

    request = models.RoutePlanRequest(
        timelimit=0.3,
        num_workers=2,
        model_input=models.RoutingInput(
            depots=[
                models.DepotModel(id="D1", location=(37.5665, 126.9780)),
                models.DepotModel(id="D2", location=(37.4979, 127.0276)),
            ],
            vehicles=[
                models.VehicleModel(id="V1", capacity=6, start_depot="D1", departure_time=7 * 3600),
                models.VehicleModel(id="V2", capacity=6, start_depot="D2", end_depot="D1", departure_time=7 * 3600),
            ],
            visits=[
                models.VisitModel(
                    id=f"C{i}",
                    location=(37.50 + i * 0.01, 127.00),
                    demand=2,
                    min_start_time=8 * 3600,
                    max_end_time=20 * 3600,
                    service_duration=600,
                )
                for i in range(6)
            ],
        ),
    )
    clone = models.RoutePlanRequest.model_validate_json(request.model_dump_json())
    assert clone.num_workers == 2
    solution = models.solve_request(clone)

    assert solution.feasible and not solution.unassigned
    assert sum(len(stops) for stops in solution.routes.values()) == 6
    assert solution.total_driving_seconds > 0
    assert solution.total_driving_meters > 0
    round_tripped = models.RouteSolution.model_validate_json(solution.model_dump_json())
    assert round_tripped == solution


def test_duplicate_ids_rejected():
    from optimaru.vehicle_routing import models

    depot = models.DepotModel(id="D", location=(0, 0))
    with pytest.raises(ValueError, match="duplicate vehicle"):
        models.RoutingInput(
            depots=[depot], vehicles=[models.VehicleModel(id="V", capacity=1, start_depot="D")] * 2, visits=[]
        )
    with pytest.raises(ValueError, match="duplicate depot"):
        models.RoutingInput(depots=[depot, depot], vehicles=[], visits=[])
    with pytest.raises(ValueError, match="share the ids"):
        models.RoutingInput(depots=[depot], vehicles=[], visits=[models.VisitModel(id="D", location=(0, 0))])


def test_unknown_depots_and_profiles_are_refused():
    depots = [models.DepotModel(id="D", location=(0, 0))]
    visits = [models.VisitModel(id="C", location=(0, 1))]

    def routing(vehicle, **fields):
        return models.RoutingInput(depots=depots, vehicles=[vehicle], visits=visits, **fields)

    with pytest.raises(ValueError, match="unknown start_depot 'X'"):
        routing(models.VehicleModel(id="V", capacity=1, start_depot="X"))
    with pytest.raises(ValueError, match="unknown end_depot 'X'"):
        routing(models.VehicleModel(id="V", capacity=1, start_depot="D", end_depot="X"))
    with pytest.raises(ValueError, match="no profiles are"):
        routing(models.VehicleModel(id="V", capacity=1, start_depot="D", profile="truck"))
    car = {"car": models.TravelProfile()}
    with pytest.raises(ValueError, match="unknown profile 'default'"):
        routing(models.VehicleModel(id="V", capacity=1, start_depot="D"), profiles=car)
    with pytest.raises(ValueError, match="unknown places \\['Y'\\]"):
        routing(
            models.VehicleModel(id="V", capacity=1, start_depot="D"),
            profiles={"default": models.TravelProfile(distances={"D": {"Y": 3}})},
        )
    with pytest.raises(ValueError, match="non-negative"):
        models.TravelProfile(durations={"D": {"C": -1}})
    with pytest.raises(ValueError, match="durations is empty"):
        models.TravelProfile(durations={})
    with pytest.raises(ValueError, match="return_to_depot"):
        models.VehicleModel(id="V", capacity=1, start_depot="D", return_to_depot=True)


def test_profiles_become_matrices_and_missing_pairs_cost_the_missing_value():
    data = models.RoutingInput(
        depots=[models.DepotModel(id="D", location=(0, 0))],
        vehicles=[
            models.VehicleModel(id="V", capacity=1, start_depot="D", profile="car"),
            models.VehicleModel(id="W", capacity=1, start_depot="D", profile="bike"),
        ],
        visits=[models.VisitModel(id="C", location=(80, 170), demand=1)],
        profiles={
            "car": models.TravelProfile(durations={"D": {"C": 7}, "C": {"D": 11}}, distances={"D": {"C": 70}}),
            "bike": models.TravelProfile(durations={"D": {"C": 30}}, missing_value=999),
        },
    )
    plan = models.to_plan(data)
    assert plan.profile_count == 2
    assert [vehicle.profile_index for vehicle in plan.vehicles] == [0, 1]
    # Locations: the depots, then the visits.
    assert (plan.driving_time(0, 1, 0), plan.driving_time(1, 0, 0)) == (7, 11)
    assert (plan.driving_time(0, 1, 1), plan.driving_time(1, 0, 1)) == (30, 999)
    assert plan.driving_time(1, 1, 1) == 0  # a place to itself, unless given
    assert (plan.driving_distance(0, 1, 0), plan.driving_distance(1, 0, 0)) == (70, models.MISSING_TRAVEL_VALUE)
    # The bike gives no distances: straight lines between the coordinates.
    assert plan.driving_distance(0, 1, 1) == plan.driving_distance(1, 0, 1) > 1_000_000


def test_the_compiled_plan_refuses_bad_profiles_and_depots():
    locations = [vrp.Location(0, 0), vrp.Location(0, 1)]
    visits = [vrp.Visit("C", "C", 1, 1, 0, 3600, 0)]

    def plan(vehicle, **matrices):
        return vrp.VehicleRoutePlan("bad", locations, [vehicle], visits, **matrices)

    with pytest.raises(ValueError, match="profile_index 1 is not one of the 1 profiles"):
        plan(vrp.Vehicle("V", 1, 0, profile_index=1))
    with pytest.raises(ValueError, match="out of range"):
        plan(vrp.Vehicle("V", 1, 0, end_location_index=-2))
    with pytest.raises(ValueError, match="out of range"):
        plan(vrp.Vehicle("V", 1, 2))
    with pytest.raises(ValueError, match="square"):
        plan(vrp.Vehicle("V", 1, 0), durations=[[[0, 1]]])
    two = plan(vrp.Vehicle("V", 1, 0, profile_index=1), durations=[None, [[0, 5], [6, 0]]])
    assert two.profile_count == 2
    assert (two.driving_time(0, 1, 1), two.driving_distance(0, 1, 1)) == (5, two.driving_distance(0, 1, 0))


def test_without_profiles_every_vehicle_drives_straight_lines():
    data = models.RoutingInput(
        depots=[models.DepotModel(id="D", location=(37.5, 127.0))],
        vehicles=[models.VehicleModel(id="V", capacity=1, start_depot="D")],
        visits=[models.VisitModel(id="C", location=(37.51, 127.0), demand=1)],
    )
    plan = models.to_plan(data)
    assert plan.profile_count == 1
    meters = plan.driving_distance(0, 1)
    assert 1_100 < meters < 1_120  # 0.01 degrees of latitude
    assert plan.driving_time(0, 1) == round(meters / 50 * 3.6)  # at 50 km/h


def _line(vehicle_fields, durations, *, constraints=None, depots=("S", "E")):
    """One vehicle and two visits, A and B; `durations` keyed by place id."""
    request = models.RoutePlanRequest(
        timelimit=0.2,
        model_input=models.RoutingInput(
            depots=[models.DepotModel(id=name, location=(0, 0)) for name in depots],
            vehicles=[models.VehicleModel(id="V", capacity=5, start_depot="S", **vehicle_fields)],
            visits=[
                models.VisitModel(id="A", location=(0, 1), service_duration=60),
                models.VisitModel(id="B", location=(0, 2), service_duration=60),
            ],
            profiles={"default": models.TravelProfile(durations=durations)},
        ),
    )
    return models.solve_request(request, constraints=constraints)


# start -> A 10, start -> B 30, A <-> B 20, B -> start 100, A -> start 10: one way A,B
# costs 30 and B,A costs 50; closing the route makes A,B cost 130 and B,A only 60.
ROUND_TRIP = {"S": {"A": 10, "B": 30}, "A": {"B": 20, "S": 10}, "B": {"A": 20, "S": 100}}


def test_routes_are_open_unless_the_vehicle_has_an_end_depot():
    """One-way by default: the route ends at the last visit and no closing leg is scored."""
    one_way = _line({}, ROUND_TRIP)
    assert one_way.feasible and not one_way.unassigned
    assert [stop.visit_id for stop in one_way.routes["V"]] == ["A", "B"]
    assert one_way.total_driving_seconds == 30
    assert one_way.score == "0hard/0medium/-30soft"
    # Post-processed per-vehicle figures: A arrives at 10 and leaves at 70,
    # B arrives at 90 and leaves at 150; the open route ends there.
    assert one_way.vehicle_metrics["V"].model_dump() == {
        "stops": 2,
        "load": 2,
        "driving_seconds": 30,
        "driving_meters": one_way.total_driving_meters,
        "departure_time": 0,
        "end_time": 150,
        "route_duration_seconds": 150,
        "waiting_seconds": 0,
        "service_seconds": 120,
    }

    round_trip = _line({"end_depot": "S"}, ROUND_TRIP)
    assert round_trip.feasible and not round_trip.unassigned
    assert [stop.visit_id for stop in round_trip.routes["V"]] == ["B", "A"]
    assert round_trip.total_driving_seconds == 60
    assert round_trip.score == "0hard/0medium/-60soft"
    # B: arrive 30, leave 90; A: arrive 110, leave 170; 10 more back to the start.
    closed = round_trip.vehicle_metrics["V"]
    assert (closed.driving_seconds, closed.end_time) == (60, 180)

    vehicle = vrp.Vehicle("V", 5, 0)
    assert (vehicle.end_location_index, vehicle.profile_index) == (-1, 0)
    vehicle.end_location_index = 0
    assert vehicle.end_location_index == 0


def test_a_route_may_end_at_another_depot():
    # The end depot E is 5 from A and 500 from B, so the route turns round: B first.
    durations = ROUND_TRIP | {"A": {"B": 20, "E": 5}, "B": {"A": 20, "E": 500}}
    solution = _line({"end_depot": "E"}, durations)
    assert [stop.visit_id for stop in solution.routes["V"]] == ["B", "A"]
    assert solution.total_driving_seconds == 30 + 20 + 5
    assert solution.vehicle_metrics["V"].end_time == 30 + 60 + 20 + 60 + 5


def test_an_idle_vehicle_drives_nothing_even_between_two_depots():
    request = models.RoutePlanRequest(
        timelimit=0.1,
        model_input=models.RoutingInput(
            depots=[models.DepotModel(id="S", location=(0, 0)), models.DepotModel(id="E", location=(0, 1))],
            vehicles=[models.VehicleModel(id="V", capacity=5, start_depot="S", end_depot="E")],
            visits=[],
        ),
    )
    for constraints in (None, get_constraints()):
        solution = models.solve_request(request, constraints=constraints)
        assert solution.score == "0hard/0medium/0soft"
        assert solution.vehicle_metrics["V"].driving_seconds == 0


def test_a_missing_pair_is_avoided():
    # A -> B is not given: it costs the missing value, so B goes first although A is nearer.
    durations = {"S": {"A": 10, "B": 30}, "B": {"A": 20}}
    solution = _line({}, durations)
    assert [stop.visit_id for stop in solution.routes["V"]] == ["B", "A"]
    assert solution.total_driving_seconds == 50


def test_each_vehicle_drives_on_its_profile():
    """Two vehicles from one depot: the bike is slow, so the car takes both visits."""
    places = ["D", "A", "B"]
    request = models.RoutePlanRequest(
        timelimit=0.2,
        model_input=models.RoutingInput(
            depots=[models.DepotModel(id="D", location=(0, 0))],
            vehicles=[
                models.VehicleModel(id="bike", capacity=5, start_depot="D", profile="bike"),
                models.VehicleModel(id="car", capacity=5, start_depot="D", profile="car"),
            ],
            visits=[models.VisitModel(id=name, location=(0, 1), service_duration=60) for name in "AB"],
            profiles={
                "car": models.TravelProfile(durations={a: {b: 10 for b in places} for a in places}),
                "bike": models.TravelProfile(durations={a: {b: 40 for b in places} for a in places}),
            },
        ),
    )
    for constraints in (None, get_constraints()):
        solution = models.solve_request(request, constraints=constraints)
        assert [stop.visit_id for stop in solution.routes["bike"]] == []
        assert sorted(stop.visit_id for stop in solution.routes["car"]) == ["A", "B"]
        assert solution.total_driving_seconds == 20


def test_travel_distance_cost_prices_the_distance_matrix():
    # Seconds prefer A then B (30); meters prefer B then A (2 + 2 against 100 + 2).
    meters = {"S": {"A": 100, "B": 2}, "A": {"B": 2}, "B": {"A": 2}}
    picks = [UnassignedVisitsCost(), TravelDistanceCost(weight=1)]
    request = models.RoutePlanRequest(
        timelimit=0.2,
        model_input=models.RoutingInput(
            depots=[models.DepotModel(id="S", location=(0, 0))],
            vehicles=[models.VehicleModel(id="V", capacity=5, start_depot="S")],
            visits=[models.VisitModel(id=name, location=(0, 1), service_duration=60) for name in "AB"],
            profiles={"default": models.TravelProfile(durations=ROUND_TRIP, distances=meters)},
        ),
    )
    by_time = models.solve_request(request)
    assert [stop.visit_id for stop in by_time.routes["V"]] == ["A", "B"]
    assert by_time.total_driving_meters == 102
    by_distance = models.solve_request(request, constraints=picks)
    assert [stop.visit_id for stop in by_distance.routes["V"]] == ["B", "A"]
    assert by_distance.total_driving_meters == 4
    assert by_distance.score == "0hard/0medium/-4soft"


def _steps(steps):
    config = vrp.SolverConfig()
    config.step_count_limit = steps
    return config


def test_package_solve_solves_in_place_and_returns_the_plan():
    plan = small_plan()
    config = _steps(2000)
    assert vrp.solve(plan, config=config, random_seed=7, full_assert=True) is plan
    assert plan.score.hard == 0
    # The keyword overrides go to a copy of the caller's config.
    assert config.full_assert is False


def test_package_solve_with_constraints_equals_solve_with_constraints():
    from optimaru.vehicle_routing.constraints import get_constraints

    by_package, by_solver = small_plan(), small_plan()
    vrp.solve(by_package, config=_steps(2000), random_seed=7, constraints=get_constraints())
    config = _steps(2000)
    config.random_seed = 7
    vrp.Solver(config).solve_with_constraints(by_solver, get_constraints())
    assert str(by_package.score) == str(by_solver.score)


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        ("parallel_solver_count", 0, "must be positive"),
        ("late_acceptance_size", -1, "must be >= 0"),
        ("ruin_recreate_rewards", [1.0, 2.0], "exactly 4 values"),
    ],
)
def test_config_rejects_out_of_range_values(field, value, message):
    with pytest.raises(ValueError, match=message):
        setattr(vrp.SolverConfig(), field, value)


def test_package_solve_forwards_the_stop_and_the_callback(monkeypatch):
    seen = {}

    class Recorder:
        def __init__(self, config):
            seen["limit"] = config.best_score_limit
            seen["callback"] = config.on_new_best

        def solve(self, problem):
            seen["solved"] = problem

    monkeypatch.setattr(vrp, "Solver", Recorder)
    callback = lambda step, score: None  # noqa: E731
    problem = small_plan()
    vrp.solve(problem, stop_at_first_feasible=True, on_new_best=callback)
    assert seen["limit"] == (0, -(2**63), -(2**63))
    assert seen["callback"] is not None and seen["solved"] is problem


def test_package_solve_reports_new_bests():
    calls = []
    vrp.solve(small_plan(), config=_steps(300), on_new_best=lambda step, score: calls.append(step))
    assert calls


def _new_bests(acceptor) -> list:
    config = vrp.SolverConfig()
    config.step_count_limit = 4000
    config.random_seed = 5
    config.acceptor_type = acceptor
    calls = []
    vrp.solve(_random_plan(), config=config, on_new_best=lambda step, score: calls.append(step))
    return calls


def test_simulated_annealing_is_not_hill_climbing():
    """Simulated annealing starts at the automatic temperature (1% of the start's soft level):
    at the engine's own (0, 0) it took no worsening move, which is hill climbing."""
    assert _new_bests(vrp.AcceptorType.SIMULATED_ANNEALING) != _new_bests(vrp.AcceptorType.HILL_CLIMBING)

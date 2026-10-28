"""Correctness tests for the vrp_routing package.

Requires the compiled extension: python3 setup.py build_ext --inplace
"""

import pytest
import vrp_routing as vrp
from vrp_routing import models


def test_request_solver_parameters_are_applied():
    config = vrp.SolverConfig()
    models._apply_solver_parameters(
        config, {"late_acceptance_size": 7, "ruin_recreate": False}
    )
    assert config.late_acceptance_size == 7
    assert config.ruin_recreate is False
    with pytest.raises(ValueError, match="unknown solver parameter"):
        models._apply_solver_parameters(config, {"not_a_parameter": 1})


def test_granular_descent_parameters_are_public():
    config = vrp.SolverConfig()
    assert config.granular_descent is True
    config.granular_descent = False
    config.granular_descent_max_segment = 3
    config.granular_descent_candidate_routes = 6
    assert not config.granular_descent
    assert config.granular_descent_max_segment == 3
    assert config.granular_descent_candidate_routes == 6


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
    models = pytest.importorskip("vrp_routing.models")

    request = models.RoutePlanRequest(
        timelimit=0.3,
        num_workers=2,
        model_input=models.RoutingInput(
            vehicles=[
                models.VehicleModel(
                    id="V1",
                    capacity=6,
                    depot=(37.5665, 126.9780),
                    departure_time=7 * 3600,
                ),
                models.VehicleModel(
                    id="V2",
                    capacity=6,
                    depot=(37.4979, 127.0276),
                    departure_time=7 * 3600,
                ),
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
    round_tripped = models.RouteSolution.model_validate_json(solution.model_dump_json())
    assert round_tripped == solution


def test_duplicate_ids_rejected():
    models = pytest.importorskip("vrp_routing.models")
    with pytest.raises(ValueError, match="duplicate"):
        models.RoutingInput(
            vehicles=[models.VehicleModel(id="V", capacity=1, depot=(0, 0))] * 2,
            visits=[],
        )


def test_request_uses_explicit_driving_matrix():
    data = models.RoutingInput(
        vehicles=[models.VehicleModel(id="V", capacity=1, depot=(0, 0))],
        visits=[models.VisitModel(id="C", location=(80, 170), demand=1)],
        driving_seconds=[[0, 7], [11, 0]],
    )
    plan = models.to_plan(data)
    assert plan.driving_time(0, 1) == 7
    assert plan.driving_time(1, 0) == 11

    with pytest.raises(ValueError, match="square matrix"):
        models.RoutingInput(
            vehicles=data.vehicles,
            visits=data.visits,
            driving_seconds=[[0]],
        )


def test_routes_are_open_unless_the_vehicle_returns_to_depot():
    """기본은 편도: 마지막 방문지에서 끝나고 복귀 구간은 채점되지 않는다."""

    def solve(return_to_depot: bool):
        request = models.RoutePlanRequest(
            timelimit=0.2,
            model_input=models.RoutingInput(
                vehicles=[
                    models.VehicleModel(
                        id="V",
                        capacity=5,
                        depot=(0, 0),
                        return_to_depot=return_to_depot,
                    )
                ],
                visits=[
                    models.VisitModel(id="A", location=(0, 1), service_duration=60),
                    models.VisitModel(id="B", location=(0, 2), service_duration=60),
                ],
                # depot -> A 10, depot -> B 30, A <-> B 20, B -> depot 100:
                # one way A,B costs 30 and B,A costs 50; closing the route makes
                # A,B cost 130 and B,A only 60, so the flag flips the best order.
                driving_seconds=[[0, 10, 30], [10, 0, 20], [100, 20, 0]],
            ),
        )
        plan = models.to_plan(request.model_input)
        assert plan.vehicles[0].return_to_depot is return_to_depot
        return models.solve_request(request)

    one_way = solve(False)
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
        "departure_time": 0,
        "end_time": 150,
        "route_duration_seconds": 150,
        "waiting_seconds": 0,
        "service_seconds": 120,
        "return_to_depot": False,
    }

    round_trip = solve(True)
    assert round_trip.feasible and not round_trip.unassigned
    assert [stop.visit_id for stop in round_trip.routes["V"]] == ["B", "A"]
    assert round_trip.total_driving_seconds == 60
    assert round_trip.score == "0hard/0medium/-60soft"
    # B: arrive 30, leave 90; A: arrive 110, leave 170; 10 more back home.
    closed = round_trip.vehicle_metrics["V"]
    assert (closed.driving_seconds, closed.end_time, closed.return_to_depot) == (
        60,
        180,
        True,
    )

    vehicle = vrp.Vehicle("V", 5, 0)
    assert vehicle.return_to_depot is False
    vehicle.return_to_depot = True
    assert vehicle.return_to_depot is True

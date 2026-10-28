"""Tests for the pdp_routing bindings.

The constraint semantics are covered exhaustively in the C++ suite
(`cpp/test/pdp_test.cpp`); these check that the Python surface exposes them
faithfully and that solving through the bindings produces a coherent plan.
"""

from __future__ import annotations

import pdp_routing as pdp
import pytest
from pdp_routing import models


def test_request_solver_parameters_are_applied():
    config = pdp.SolverConfig()
    models._apply_solver_parameters(
        config,
        {
            "late_acceptance_size": 7,
            "ruin_recreate": False,
            "acceptor_type": "HILL_CLIMBING",
            "best_score_limit": None,
        },
    )
    assert config.late_acceptance_size == 7
    assert config.ruin_recreate is False
    assert config.acceptor_type == pdp.AcceptorType.HILL_CLIMBING
    with pytest.raises(ValueError, match="controlled by the solve request"):
        models._apply_solver_parameters(config, {"random_seed": 3})


def hour_minute(hour: int, minute: int) -> int:
    return hour * 3600 + minute * 60


def build_two_request_plan(demand: int = 4, capacity: int = 10) -> pdp.PdpRoutePlan:
    """One vehicle, two requests, laid out so every expectation is exact."""
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


def test_ordered_pair_is_feasible():
    plan = build_two_request_plan()
    plan.vehicles[0].node_indices = [0, 1, 2, 3]
    plan.rebuild_unassigned_pool()
    score = pdp.calculate_score(plan)
    assert score.hard == 0
    assert score.medium == 0
    assert score.soft < 0
    assert score.is_feasible()


def test_dropoff_before_pickup_is_infeasible():
    plan = build_two_request_plan()
    plan.vehicles[0].node_indices = [1, 0, 2, 3]  # d1 before p1
    plan.rebuild_unassigned_pool()
    assert pdp.calculate_score(plan).hard < 0


def test_half_routed_request_is_infeasible():
    plan = build_two_request_plan()
    plan.vehicles[0].node_indices = [0, 2, 3]  # p1 routed, d1 dropped
    plan.rebuild_unassigned_pool()
    assert pdp.calculate_score(plan).hard < 0


def test_fully_dropped_request_is_medium_not_hard():
    plan = build_two_request_plan()
    plan.vehicles[0].node_indices = [2, 3]  # request 1 dropped entirely
    plan.rebuild_unassigned_pool()
    score = pdp.calculate_score(plan)
    assert score.hard == 0
    assert score.medium == -10  # 5 + 5 service minutes
    assert "r1" in plan.unassigned_delivery_ids()


def test_capacity_is_a_running_peak_not_a_route_total():
    """Two overlapping requests break capacity; the same two in sequence do not."""
    overlapping = build_two_request_plan(demand=6)
    overlapping.vehicles[0].node_indices = [0, 2, 1, 3]  # p1 p2 d1 d2
    overlapping.rebuild_unassigned_pool()
    assert pdp.calculate_score(overlapping).hard == -2  # 12 carried vs capacity 10

    sequential = build_two_request_plan(demand=6)
    sequential.vehicles[0].node_indices = [0, 1, 2, 3]  # p1 d1 p2 d2
    sequential.rebuild_unassigned_pool()
    assert pdp.calculate_score(sequential).hard == 0


def test_solve_produces_a_feasible_coherent_plan():
    plan = pdp.build_random_instance(request_count=20, vehicle_count=3, seed=7)

    config = pdp.SolverConfig()
    config.spent_limit_seconds = 3.0
    config.random_seed = 7
    pdp.Solver(config).solve(plan)

    assert plan.score.is_feasible()

    # Every stop is routed exactly once, or sits in the unassigned pool.
    seen: list[int] = []
    for vehicle in plan.vehicles:
        seen.extend(vehicle.node_indices)
    seen.extend(plan.unassigned_node_indices)
    assert sorted(seen) == list(range(len(plan.nodes)))

    # The reported score survives a from-scratch recompute.
    reported = (plan.score.hard, plan.score.medium, plan.score.soft)
    recomputed = pdp.calculate_score(plan)
    assert (recomputed.hard, recomputed.medium, recomputed.soft) == reported


def test_alns_knobs_are_settable():
    config = pdp.SolverConfig()
    config.acceptor_type = pdp.AcceptorType.RECORD_TO_RECORD_TRAVEL
    config.ruin_recreate_selection_type = pdp.OperatorSelectionType.ALPHA_UCB
    config.regret_k = 4
    config.blink_rate = 0.02
    config.sisr_ruin = False
    config.pair_moves = False
    assert config.regret_k == 4
    assert config.acceptor_type == pdp.AcceptorType.RECORD_TO_RECORD_TRAVEL

    with pytest.raises(TypeError):
        config.regret_k = "three"


def test_request_uses_explicit_driving_matrix():
    data = models.PdpInput(
        vehicles=[models.VehicleModel(id="V", capacity=1, home=(0, 0))],
        deliveries=[
            models.DeliveryModel(
                id="R",
                demand=1,
                pickup=models.StopModel(id="P", location=(80, 170)),
                dropoff=models.StopModel(id="D", location=(-80, -170)),
            )
        ],
        driving_seconds=[[0, 3, 5], [7, 0, 11], [13, 17, 0]],
    )
    plan = models.to_plan(data)
    assert plan.driving_time(0, 1) == 3
    assert plan.driving_time(1, 2) == 11

    with pytest.raises(ValueError, match="non-negative"):
        models.PdpInput(
            vehicles=data.vehicles,
            deliveries=data.deliveries,
            driving_seconds=[[0, 3, 5], [7, 0, -1], [13, 17, 0]],
        )


def test_json_vehicles_default_to_twenty_stops_and_open_routes():
    """JSON contract defaults: 20-stop limit, one-way. C++ vehicles have both off."""
    vehicle = models.VehicleModel(id="V", capacity=1, home=(0, 0))
    assert vehicle.max_stops == 20
    assert vehicle.return_to_depot is False
    core = models.to_plan(models.PdpInput(vehicles=[vehicle], deliveries=[])).vehicles[
        0
    ]
    assert core.max_stops == 20
    assert pdp.Vehicle("V", 1, 0).max_stops == 0


def test_routes_are_open_unless_the_vehicle_returns_to_depot():
    """One-way by default: the route ends at the dropoff and no return leg is scored."""

    def solve(return_to_depot: bool):
        request = models.PdpRequest(
            timelimit=0.2,
            model_input=models.PdpInput(
                vehicles=[
                    models.VehicleModel(
                        id="V", capacity=1, home=(0, 0), return_to_depot=return_to_depot
                    )
                ],
                deliveries=[
                    models.DeliveryModel(
                        id="R",
                        demand=1,
                        # Service minutes make dropping the request cost medium.
                        pickup=models.StopModel(
                            id="P", location=(0, 1), service_duration=60
                        ),
                        dropoff=models.StopModel(
                            id="D", location=(0, 2), service_duration=60
                        ),
                    )
                ],
                # home -> P 10, P -> D 20, D -> home 100 (asymmetric on purpose).
                driving_seconds=[[0, 10, 30], [10, 0, 20], [100, 20, 0]],
            ),
        )
        plan = models.to_plan(request.model_input)
        assert plan.vehicles[0].return_to_depot is return_to_depot
        return models.solve_request(request)

    one_way = solve(False)
    assert one_way.feasible and not one_way.unassigned
    assert [stop.stop_id for stop in one_way.routes["V"]] == ["P", "D"]
    assert one_way.total_driving_seconds == 30
    assert one_way.score == "0hard/0medium/-30soft"
    # Post-processed per-vehicle figures: P arrives at 10 and leaves at 70,
    # D arrives at 90 and leaves at 150; the open route ends there.
    assert one_way.vehicle_metrics["V"].model_dump() == {
        "stops": 2,
        "requests": 1,
        "peak_load": 1,
        "delivered": 1,
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
    assert [stop.stop_id for stop in round_trip.routes["V"]] == ["P", "D"]
    assert round_trip.total_driving_seconds == 130
    assert round_trip.score == "0hard/0medium/-130soft"
    closed = round_trip.vehicle_metrics["V"]
    assert (closed.driving_seconds, closed.end_time, closed.return_to_depot) == (
        130,
        250,
        True,
    )

    vehicle = pdp.Vehicle("V", 1, 0)
    assert vehicle.return_to_depot is False
    vehicle.return_to_depot = True
    assert vehicle.return_to_depot is True

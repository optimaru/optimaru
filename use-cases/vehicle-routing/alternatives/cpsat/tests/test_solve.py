from vrp_routing.models import RoutePlanRequest, RoutingInput, VehicleModel, VisitModel
from vehicle_routing_cpsat import catalog, solve_request


def test_catalog_exposes_route_policies() -> None:
    rows = catalog()
    assert {row["name"] for row in rows} == {
        "vehicle_capacity",
        "service_finished_after_max_end_time",
        "max_travel_time",
        "max_route_duration",
        "vehicle_end_time",
        "max_stops",
        "max_route_radius",
        "maximize_visits_assigned",
        "min_capacity_utilization",
        "minimize_travel_time",
        "minimize_route_duration",
        "minimize_latency",
        "vehicle_fixed_cost",
    }
    assert all(row["label_ko"] and row["label_en"] for row in rows)
    assert all(row["description_ko"] and row["description_en"] for row in rows)


def test_capacity_splits_visits_across_vehicles() -> None:
    request = RoutePlanRequest(
        timelimit=2,
        model_input=RoutingInput(
            vehicles=[
                VehicleModel(id="a", capacity=1, depot=(37.0, 127.0)),
                VehicleModel(id="b", capacity=1, depot=(37.0, 127.0)),
            ],
            visits=[
                VisitModel(
                    id="x", location=(37.01, 127.0), demand=1, service_duration=60
                ),
                VisitModel(
                    id="y", location=(37.02, 127.0), demand=1, service_duration=60
                ),
            ],
        ),
    )

    solved = solve_request(request)

    assert solved.feasible
    assert not solved.unassigned
    assert all(len(route) == 1 for route in solved.routes.values())


def test_max_stops_can_leave_a_visit_unassigned() -> None:
    request = RoutePlanRequest(
        timelimit=2,
        model_input=RoutingInput(
            vehicles=[
                VehicleModel(id="a", capacity=10, depot=(37.0, 127.0), max_stops=1)
            ],
            visits=[
                VisitModel(
                    id="x", location=(37.01, 127.0), demand=1, service_duration=60
                ),
                VisitModel(
                    id="y", location=(37.02, 127.0), demand=1, service_duration=60
                ),
            ],
        ),
    )

    solved = solve_request(request)

    assert solved.feasible
    assert len(solved.routes["a"]) == 1
    assert len(solved.unassigned) == 1

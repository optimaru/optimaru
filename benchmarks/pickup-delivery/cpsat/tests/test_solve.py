from pdp_routing.models import (
    DeliveryModel,
    PdpInput,
    PdpRequest,
    StopModel,
    VehicleModel,
    solve_request as solve_metaheuristic_request,
)
from pdp_routing.constraints import vehicle_capacity
from pickup_delivery_cpsat import catalog, solve_request


def test_catalog_exposes_route_policies() -> None:
    rows = catalog()
    assert all(row["label_ko"] and row["label_en"] for row in rows)
    assert all(row["description_ko"] and row["description_en"] for row in rows)
    assert {row["name"] for row in rows} == {
        "vehicle_capacity",
        "service_finished_after_max_end_time",
        "max_travel_time",
        "max_route_duration",
        "vehicle_end_time",
        "max_stops",
        "max_route_radius",
        "unassigned_requests",
        "min_capacity_utilization",
        "minimize_travel_time",
        "minimize_route_duration",
        "minimize_latency",
        "vehicle_fixed_cost",
    }


def test_pickup_precedes_dropoff() -> None:
    request = PdpRequest(
        timelimit=2,
        model_input=PdpInput(
            vehicles=[VehicleModel(id="v", capacity=2, home=(37.0, 127.0))],
            deliveries=[
                DeliveryModel(
                    id="d",
                    demand=2,
                    pickup=StopModel(
                        id="p", location=(37.01, 127.0), service_duration=60
                    ),
                    dropoff=StopModel(
                        id="q", location=(37.02, 127.0), service_duration=60
                    ),
                )
            ],
        ),
    )

    solved = solve_request(request)

    assert solved.feasible
    assert [stop.pickup for stop in solved.routes["v"]] == [True, False]

    meta = solve_metaheuristic_request(
        request.model_copy(update={"timelimit": 0.1}),
        constraints=[vehicle_capacity],
    )
    assert set(meta.routes) == {"v"}
    assert meta.score


def test_max_stops_can_leave_a_delivery_unassigned() -> None:
    request = PdpRequest(
        timelimit=2,
        model_input=PdpInput(
            vehicles=[
                VehicleModel(id="v", capacity=10, home=(37.0, 127.0), max_stops=2)
            ],
            deliveries=[
                DeliveryModel(
                    id=f"d{index}",
                    demand=1,
                    pickup=StopModel(
                        id=f"p{index}",
                        location=(37.01 + index * 0.01, 127.0),
                        service_duration=60,
                    ),
                    dropoff=StopModel(
                        id=f"q{index}",
                        location=(37.02 + index * 0.01, 127.0),
                        service_duration=60,
                    ),
                )
                for index in range(2)
            ],
        ),
    )

    solved = solve_request(request)

    assert solved.feasible
    assert len(solved.routes["v"]) == 2
    assert len(solved.unassigned) == 1

"""Small hand-calculated constraint checks shared by both PDP backends."""

import math

import pdp_routing as pdp
import pytest
from pdp_routing.constraints import get_constraints, score
from pdp_routing.models import (
    DeliveryModel,
    PdpInput,
    PdpRequest,
    StopModel,
    VehicleModel,
    to_plan,
)
from pickup_delivery_cpsat import solve_request


# Pinned route: depot -> pickup -> dropoff -> depot. Its three legs take
# 80 + 80 + 160 = 320 seconds when the vehicle returns to its depot. Waiting at
# pickup makes the route duration 590 seconds and arrival latency
# 80 + 340 = 420 seconds. Routes are open by default, so the cases whose
# numbers include the closing leg set return_to_depot explicitly; the
# open-route figures (160 driven, route ends at the dropoff) are checked
# separately.
RADIUS_OVER_METERS = 2 * 6_371_000 * math.pi * 0.02 / 360 - 2_000


CASES = [
    ("vehicle_capacity", "hard", 2, 2, {"capacity": 10}, 12, {}, ("P", "D")),
    (
        "service_finished_after_max_end_time",
        "hard",
        2,
        2,
        {},
        3,
        {"max_end_times": (200, 480)},
        ("P", "D"),
    ),
    (
        "max_travel_time",
        "hard",
        20,
        20,
        {"max_travel_time": 300, "return_to_depot": True},
        3,
        {},
        ("P", "D"),
    ),
    (
        "max_route_duration",
        "hard",
        90,
        90,
        {"max_route_duration": 500, "return_to_depot": True},
        3,
        {},
        ("P", "D"),
    ),
    (
        "vehicle_end_time",
        "hard",
        40,
        40,
        {"max_end_time": 650, "return_to_depot": True},
        3,
        {},
        ("P", "D"),
    ),
    ("max_stops", "hard", 1, 1, {"max_stops": 1}, 3, {}, ("P", "D")),
    (
        "max_route_radius",
        "hard",
        RADIUS_OVER_METERS,
        224,
        {"max_route_radius_meters": 2_000},
        3,
        {},
        ("P", "D"),
    ),
    ("unassigned_requests", "medium", 2, 2, {}, 3, {}, ()),
    (
        "min_capacity_utilization",
        "soft",
        5,
        5,
        {"capacity": 10, "min_load_ratio": 0.8},
        3,
        {},
        ("P", "D"),
    ),
    (
        "minimize_travel_time",
        "soft",
        320,
        320,
        {"return_to_depot": True},
        3,
        {},
        ("P", "D"),
    ),
    (
        "minimize_route_duration",
        "soft",
        590,
        590,
        {"return_to_depot": True},
        3,
        {},
        ("P", "D"),
    ),
    ("minimize_latency", "soft", 420, 420, {}, 3, {}, ("P", "D")),
    ("vehicle_fixed_cost", "soft", 25, 25, {"fixed_cost": 25}, 3, {}, ("P", "D")),
]


def _request(vehicle_fields, demand, case_fields, *, two_vehicles=False) -> PdpRequest:
    vehicle = {"id": "V1", "capacity": 20, "home": (0.0, 0.0), "departure_time": 100}
    vehicle.update(vehicle_fields)
    vehicles = [VehicleModel(**vehicle)]
    if two_vehicles:
        vehicles.append(
            VehicleModel(id="V2", capacity=20, home=(0.0, 0.0), departure_time=100)
        )
    max_end_times = case_fields.get("max_end_times", (10_000, 10_000))
    return PdpRequest(
        timelimit=1,
        model_input=PdpInput(
            vehicles=vehicles,
            deliveries=[
                DeliveryModel(
                    id="R",
                    demand=demand,
                    pickup=StopModel(
                        id="P",
                        location=(0.0, 0.01),
                        min_start_time=300,
                        max_end_time=max_end_times[0],
                        service_duration=60,
                    ),
                    dropoff=StopModel(
                        id="D",
                        location=(0.0, 0.02),
                        max_end_time=max_end_times[1],
                        service_duration=90,
                    ),
                )
            ],
        ),
    )


def _timefold_cost(request, routes, name):
    plan = to_plan(request.model_input)
    node_by_id = {node.id: i for i, node in enumerate(plan.nodes)}
    for vehicle in plan.vehicles:
        vehicle.node_indices = [node_by_id[node_id] for node_id in routes[vehicle.id]]
    plan.rebuild_unassigned_pool()
    raw = pdp.constraint_costs(plan)
    raw_cost = sum(raw[name]["costs"])
    default = next(
        constraint for constraint in get_constraints() if constraint.name == name
    )
    isolated = type(default)(weight=1)
    isolated_score = score(raw, [isolated])
    return raw_cost, isolated_score


def _parse_score(value: str) -> dict[str, int]:
    parsed = {}
    for part in value.split("/"):
        for level in ("hard", "medium", "soft"):
            if part.endswith(level):
                parsed[level] = int(part[: -len(level)])
    return parsed


@pytest.mark.parametrize(
    "name,level,expected_raw,expected_score,vehicle_fields,demand,case_fields,route",
    CASES,
    ids=[case[0] for case in CASES],
)
def test_constraint_matches_hand_calculation_and_other_backend(
    name,
    level,
    expected_raw,
    expected_score,
    vehicle_fields,
    demand,
    case_fields,
    route,
):
    request = _request(vehicle_fields, demand, case_fields)
    routes = {"V1": route}

    raw_cost, timefold_score = _timefold_cost(request, routes, name)
    assert raw_cost == pytest.approx(expected_raw)
    timefold_penalty = -(
        timefold_score.hard if level == "hard" else timefold_score.soft
    )
    assert timefold_penalty == pytest.approx(expected_score)

    exact = solve_request(
        request,
        constraints=[{"name": name, "weight": 1}],
        _fixed_routes=routes,
    )
    assert -_parse_score(exact.score)[level] == expected_score


@pytest.mark.parametrize(
    "name,vehicle_fields,expected",
    [
        # Open route (the default): depot -> P -> D stops at D, 80 + 80 driven.
        ("minimize_travel_time", {}, 160),
        # ... and ends when D's service does: 530 - 100 departure = 430.
        ("minimize_route_duration", {}, 430),
        # The 650 end-time limit is met without the 160-second closing leg.
        ("vehicle_end_time", {"max_end_time": 650}, 0),
        ("max_travel_time", {"max_travel_time": 300}, 0),
    ],
    ids=lambda value: value if isinstance(value, str) else None,
)
def test_open_route_skips_the_closing_leg_on_both_backends(
    name, vehicle_fields, expected
):
    """By default a vehicle does not drive back to its depot."""
    request = _request(vehicle_fields, 3, {})
    assert not request.model_input.vehicles[0].return_to_depot
    routes = {"V1": ("P", "D")}

    raw_cost, _ = _timefold_cost(request, routes, name)
    assert raw_cost == pytest.approx(expected)

    exact = solve_request(
        request,
        constraints=[{"name": name, "weight": 1}],
        _fixed_routes=routes,
    )
    level = "hard" if name in {"vehicle_end_time", "max_travel_time"} else "soft"
    assert -_parse_score(exact.score)[level] == expected
    assert exact.total_driving_seconds == 160
    # Post-processed per-vehicle figures. Stop count, load, driving and
    # service time follow from the pinned route alone; the arrival chain is
    # only pinned when the objective pushes the route end down, so the timing
    # figures (P arrives 180, waits until 300, leaves 360; D arrives 440,
    # leaves 530) are checked on that case.
    figures = exact.vehicle_metrics["V1"]
    assert (figures.stops, figures.requests, figures.peak_load) == (2, 1, 3)
    assert (figures.driving_seconds, figures.service_seconds) == (160, 150)
    if name == "minimize_route_duration":
        assert (figures.waiting_seconds, figures.end_time) == (120, 530)
        assert figures.route_duration_seconds == 430


@pytest.mark.parametrize(
    "name,routes,two_vehicles",
    [
        ("paired_request", {"V1": ("P",)}, False),
        ("same_vehicle", {"V1": ("P",), "V2": ("D",)}, True),
        ("pickup_before_dropoff", {"V1": ("D", "P")}, False),
    ],
)
def test_request_coupling_is_scored_by_timefold_and_rejected_by_cpsat(
    name, routes, two_vehicles
):
    """The exact model enforces the three unit penalties as invariants."""
    request = _request({}, 3, {}, two_vehicles=two_vehicles)
    raw_cost, _ = _timefold_cost(request, routes, name)
    assert raw_cost == 1  # exactly one broken pickup/dropoff relationship

    with pytest.raises(RuntimeError, match="INFEASIBLE"):
        solve_request(
            request,
            constraints=["minimize_travel_time"],
            _fixed_routes=routes,
        )

"""Small hand-calculated constraint checks shared by both PDP backends."""

import math

import pytest
from external_solver import NoSolutionError

import optimaru.pickup_delivery as pdp
from optimaru.pickup_delivery.constraints import get_constraints, score
from optimaru.pickup_delivery.models import (
    DeliveryModel,
    DepotModel,
    PdpInput,
    PdpRequest,
    StopModel,
    TravelProfile,
    VehicleModel,
    to_plan,
)
from pickup_delivery_cpsat.model import solve_request

# Pinned route: depot -> pickup -> dropoff -> depot. Its three legs take
# 80 + 80 + 160 = 320 seconds when the vehicle returns to its depot. Waiting at
# pickup makes the route duration 590 seconds and arrival latency
# 80 + 340 = 420 seconds. Routes are open by default, so the cases whose
# numbers include the closing leg give the vehicle an end_depot; the
# open-route figures (160 driven, route ends at the dropoff) are checked
# separately.
RADIUS_OVER_METERS = 2 * 6_371_000 * math.pi * 0.02 / 360 - 2_000


CASES = [
    ("vehicle_capacity_cost", "hard", 2, 2, {"capacity": 10}, 12, {}, ("P", "D")),
    (
        "max_end_time_cost",
        "hard",
        2,
        2,
        {},
        3,
        {"max_end_times": (200, 480)},
        ("P", "D"),
    ),
    (
        "max_travel_time_cost",
        "hard",
        20,
        20,
        {"max_travel_time": 300, "end_depot": "H"},
        3,
        {},
        ("P", "D"),
    ),
    (
        "max_route_duration_cost",
        "hard",
        90,
        90,
        {"max_route_duration": 500, "end_depot": "H"},
        3,
        {},
        ("P", "D"),
    ),
    (
        "vehicle_max_end_time_cost",
        "hard",
        40,
        40,
        {"max_end_time": 650, "end_depot": "H"},
        3,
        {},
        ("P", "D"),
    ),
    ("max_stops_cost", "hard", 1, 1, {"max_stops": 1}, 3, {}, ("P", "D")),
    (
        "max_route_radius_cost",
        "hard",
        RADIUS_OVER_METERS,
        224,
        {"max_route_radius_meters": 2_000},
        3,
        {},
        ("P", "D"),
    ),
    ("unassigned_requests_cost", "medium", 2, 2, {}, 3, {}, ()),
    (
        "min_capacity_utilization_cost",
        "soft",
        5,
        5,
        {"capacity": 10, "min_load_ratio": 0.8},
        3,
        {},
        ("P", "D"),
    ),
    (
        "travel_time_cost",
        "soft",
        320,
        320,
        {"end_depot": "H"},
        3,
        {},
        ("P", "D"),
    ),
    (
        "route_duration_cost",
        "soft",
        590,
        590,
        {"end_depot": "H"},
        3,
        {},
        ("P", "D"),
    ),
    ("latency_cost", "soft", 420, 420, {}, 3, {}, ("P", "D")),
    ("vehicle_fixed_cost", "soft", 25, 25, {"fixed_cost": 25}, 3, {}, ("P", "D")),
]


def _request(vehicle_fields, demand, case_fields, *, two_vehicles=False) -> PdpRequest:
    vehicle = {"id": "V1", "capacity": 20, "start_depot": "H", "departure_time": 100}
    vehicle.update(vehicle_fields)
    vehicles = [VehicleModel(**vehicle)]
    if two_vehicles:
        vehicles.append(VehicleModel(id="V2", capacity=20, start_depot="H", departure_time=100))
    max_end_times = case_fields.get("max_end_times", (10_000, 10_000))
    return PdpRequest(
        time_limit_seconds=1,
        model_input=PdpInput(
            depots=[DepotModel(id="H", location=(0.0, 0.0))],
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


def _engine_cost(request, routes, name):
    plan = to_plan(request.model_input)
    node_by_id = {node.id: i for i, node in enumerate(plan.nodes)}
    for vehicle in plan.vehicles:
        vehicle.node_indices = [node_by_id[node_id] for node_id in routes[vehicle.id]]
    plan.rebuild_unassigned_pool()
    raw = pdp.constraint_costs(plan)
    raw_cost = sum(raw[name]["costs"])
    default = next(constraint for constraint in get_constraints() if constraint.name == name)
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

    raw_cost, engine_score = _engine_cost(request, routes, name)
    assert raw_cost == pytest.approx(expected_raw)
    engine_penalty = -(engine_score.hard if level == "hard" else engine_score.soft)
    if level == "medium":
        # The engine score is two-level: it folds medium into soft by the
        # domain's medium_fold (the weight is in medium units, so the
        # fold is no longer part of it). Compare in medium units.
        plan = to_plan(request.model_input)
        engine_penalty /= pdp.constraint_costs(plan)[name]["medium_fold"]["unit"]
    assert engine_penalty == pytest.approx(expected_score)

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
        ("travel_time_cost", {}, 160),
        # ... and ends when D's service does: 530 - 100 departure = 430.
        ("route_duration_cost", {}, 430),
        # The 650 end-time limit is met without the 160-second closing leg.
        ("vehicle_max_end_time_cost", {"max_end_time": 650}, 0),
        ("max_travel_time_cost", {"max_travel_time": 300}, 0),
    ],
    ids=lambda value: value if isinstance(value, str) else None,
)
def test_open_route_skips_the_closing_leg_on_both_backends(name, vehicle_fields, expected):
    """By default a vehicle does not drive back to its depot."""
    request = _request(vehicle_fields, 3, {})
    assert request.model_input.vehicles[0].end_depot is None
    routes = {"V1": ("P", "D")}

    raw_cost, _ = _engine_cost(request, routes, name)
    assert raw_cost == pytest.approx(expected)

    exact = solve_request(
        request,
        constraints=[{"name": name, "weight": 1}],
        _fixed_routes=routes,
    )
    level = "hard" if name in {"vehicle_max_end_time_cost", "max_travel_time_cost"} else "soft"
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
    if name == "route_duration_cost":
        assert (figures.waiting_seconds, figures.end_time) == (120, 530)
        assert figures.route_duration_seconds == 430


@pytest.mark.parametrize(
    "name,routes,two_vehicles",
    [
        ("paired_request_cost", {"V1": ("P",)}, False),
        ("same_vehicle_cost", {"V1": ("P",), "V2": ("D",)}, True),
        ("pickup_before_dropoff_cost", {"V1": ("D", "P")}, False),
    ],
)
def test_request_coupling_is_scored_by_the_engine_and_rejected_by_cpsat(name, routes, two_vehicles):
    """The exact model enforces the three unit penalties as invariants."""
    request = _request({}, 3, {}, two_vehicles=two_vehicles)
    raw_cost, _ = _engine_cost(request, routes, name)
    assert raw_cost == 1  # exactly one broken pickup/dropoff relationship

    with pytest.raises(NoSolutionError, match="INFEASIBLE") as raised:
        solve_request(
            request,
            constraints=["travel_time_cost"],
            _fixed_routes=routes,
        )
    assert raised.value.infeasible


@pytest.mark.parametrize(
    "name,expected",
    [
        # H -> P 50, P -> D 70, D -> E (the end depot) 90; the pair D -> H is left out
        # but not driven, since the route ends at E.
        ("travel_time_cost", 210),
        ("travel_distance_cost", 400 + 600 + 800),
        # Departs 100, at P 150 waits to 300, leaves 360, at D 430, leaves 520, at E 610.
        ("route_duration_cost", 510),
    ],
)
def test_a_profile_and_another_end_depot_score_alike_on_both_backends(name, expected):
    """The vehicle drives its own profile's dicts and ends at a depot other than its start."""
    body = _request({}, 3, {}).model_dump()
    body["model_input"]["vehicles"][0] |= {"end_depot": "E", "profile": "van"}
    body["model_input"]["depots"].append({"id": "E", "location": (0.0, 0.03)})
    body["model_input"]["profiles"] = {
        "van": TravelProfile(
            durations={"H": {"P": 50}, "P": {"D": 70}, "D": {"E": 90}},
            distances={"H": {"P": 400}, "P": {"D": 600}, "D": {"E": 800}},
            missing_value=5_000,
        ).model_dump()
    }
    request = PdpRequest.model_validate(body)
    routes = {"V1": ("P", "D")}
    raw_cost, _ = _engine_cost(request, routes, name)
    assert raw_cost == pytest.approx(expected)
    exact = solve_request(request, constraints=[{"name": name, "weight": 1}], _fixed_routes=routes)
    assert -_parse_score(exact.score)["soft"] == expected

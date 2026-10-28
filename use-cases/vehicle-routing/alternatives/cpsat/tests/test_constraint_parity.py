"""Small hand-calculated constraint checks shared by both VRP backends."""

import math

import pytest
import vrp_routing as vrp
from vrp_routing.constraints import get_constraints, score
from vrp_routing.models import (
    RoutePlanRequest,
    RoutingInput,
    VehicleModel,
    VisitModel,
    to_plan,
)
from vehicle_routing_cpsat import solve_request


# At the equator, 0.01 degrees is 1,111.949 m. The engine rounds that to
# 1,112 m and then to 80 driving seconds at 50 km/h. Thus the fixed route
# depot -> A -> B -> depot drives 80 + 80 + 160 = 320 seconds when the vehicle
# returns to its depot. Routes are open by default, so the cases whose numbers
# include the closing leg set return_to_depot explicitly; the open-route
# figures (160 driven, no closing leg in the end time) are checked separately.
RADIUS_OVER_METERS = 2 * 6_371_000 * math.pi * 0.02 / 360 - 2_000


CASES = [
    ("vehicle_capacity", "hard", 2, 2, {"capacity": 10}, (7, 5), {}, ("A", "B")),
    (
        "service_finished_after_max_end_time",
        "hard",
        2,
        2,
        {},
        (2, 1),
        {"max_end_times": (200, 480)},
        ("A", "B"),
    ),
    (
        "max_travel_time",
        "hard",
        20,
        20,
        {"max_travel_time": 300, "return_to_depot": True},
        (2, 1),
        {},
        ("A", "B"),
    ),
    (
        "max_route_duration",
        "hard",
        90,
        90,
        {"max_route_duration": 500, "return_to_depot": True},
        (2, 1),
        {},
        ("A", "B"),
    ),
    (
        "vehicle_end_time",
        "hard",
        40,
        40,
        {"max_end_time": 650, "return_to_depot": True},
        (2, 1),
        {},
        ("A", "B"),
    ),
    ("max_stops", "hard", 1, 1, {"max_stops": 1}, (2, 1), {}, ("A", "B")),
    (
        "max_route_radius",
        "hard",
        RADIUS_OVER_METERS,
        224,
        {"max_route_radius_meters": 2_000},
        (2, 1),
        {},
        ("A", "B"),
    ),
    ("maximize_visits_assigned", "medium", 1, 1, {}, (2, 1), {}, ("A",)),
    (
        "min_capacity_utilization",
        "soft",
        5,
        5,
        {"capacity": 10, "min_load_ratio": 0.8},
        (2, 1),
        {},
        ("A", "B"),
    ),
    (
        "minimize_travel_time",
        "soft",
        320,
        320,
        {"return_to_depot": True},
        (2, 1),
        {},
        ("A", "B"),
    ),
    (
        "minimize_route_duration",
        "soft",
        590,
        590,
        {"return_to_depot": True},
        (2, 1),
        {},
        ("A", "B"),
    ),
    ("minimize_latency", "soft", 420, 420, {}, (2, 1), {}, ("A", "B")),
    ("vehicle_fixed_cost", "soft", 25, 25, {"fixed_cost": 25}, (2, 1), {}, ("A", "B")),
]


def _request(vehicle_fields, demands, case_fields) -> RoutePlanRequest:
    vehicle = {"id": "V", "capacity": 20, "depot": (0.0, 0.0), "departure_time": 100}
    vehicle.update(vehicle_fields)
    max_end_times = case_fields.get("max_end_times", (10_000, 10_000))
    return RoutePlanRequest(
        timelimit=1,
        model_input=RoutingInput(
            vehicles=[VehicleModel(**vehicle)],
            visits=[
                VisitModel(
                    id="A",
                    location=(0.0, 0.01),
                    demand=demands[0],
                    min_start_time=300,
                    max_end_time=max_end_times[0],
                    service_duration=60,
                ),
                VisitModel(
                    id="B",
                    location=(0.0, 0.02),
                    demand=demands[1],
                    max_end_time=max_end_times[1],
                    service_duration=90,
                ),
            ],
        ),
    )


def _timefold_cost(request, route, name):
    plan = to_plan(request.model_input)
    visit_by_id = {visit.id: i for i, visit in enumerate(plan.visits)}
    plan.vehicles[0].visit_indices = [visit_by_id[visit_id] for visit_id in route]
    plan.rebuild_unassigned_pool()
    raw = vrp.constraint_costs(plan)
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
    "name,level,expected_raw,expected_score,vehicle_fields,demands,case_fields,route",
    CASES,
    ids=[case[0] for case in CASES],
)
def test_constraint_matches_hand_calculation_and_other_backend(
    name,
    level,
    expected_raw,
    expected_score,
    vehicle_fields,
    demands,
    case_fields,
    route,
):
    """Each backend independently prices the same pinned route."""
    request = _request(vehicle_fields, demands, case_fields)

    raw_cost, timefold_score = _timefold_cost(request, route, name)
    assert raw_cost == pytest.approx(expected_raw)
    timefold_penalty = -(
        timefold_score.hard if level == "hard" else timefold_score.soft
    )
    assert timefold_penalty == pytest.approx(expected_score)

    exact = solve_request(
        request,
        constraints=[{"name": name, "weight": 1}],
        _fixed_routes={"V": route},
    )
    assert -_parse_score(exact.score)[level] == expected_score


@pytest.mark.parametrize(
    "name,vehicle_fields,expected",
    [
        # Open route (the default): depot -> A -> B stops at B, 80 + 80 driven.
        ("minimize_travel_time", {}, 160),
        # ... and ends when B's service does: 530 - 100 departure = 430.
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
    request = _request(vehicle_fields, (2, 1), {})
    assert not request.model_input.vehicles[0].return_to_depot

    raw_cost, _ = _timefold_cost(request, ("A", "B"), name)
    assert raw_cost == pytest.approx(expected)

    exact = solve_request(
        request,
        constraints=[{"name": name, "weight": 1}],
        _fixed_routes={"V": ("A", "B")},
    )
    level = "hard" if name in {"vehicle_end_time", "max_travel_time"} else "soft"
    assert -_parse_score(exact.score)[level] == expected
    assert exact.total_driving_seconds == 160
    # Post-processed per-vehicle figures. Stop count, driving and service
    # time follow from the pinned route alone; the arrival chain is only
    # pinned when the objective pushes the route end down, so the timing
    # figures (A arrives 180, waits until 300, leaves 360; B arrives 440,
    # leaves 530) are checked on that case.
    figures = exact.vehicle_metrics["V"]
    assert (figures.stops, figures.driving_seconds, figures.service_seconds) == (
        2,
        160,
        150,
    )
    if name == "minimize_route_duration":
        assert (figures.waiting_seconds, figures.end_time) == (120, 530)
        assert figures.route_duration_seconds == 430

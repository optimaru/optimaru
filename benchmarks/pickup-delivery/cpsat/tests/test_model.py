"""The kept CP-SAT model against the engine: the same catalog and defaults, and the same costs for
pinned routes on the 0.6 input (a travel profile and an end depot other than the start)."""

import math

import pytest

import optimaru.pickup_delivery as engine
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
from pickup_delivery_cpsat.model import catalog, solve_request

# Engine constraints the CP-SAT model has no plugin for.
ENGINE_ONLY = {
    "enclosing_circle_radius_cost",
    "squared_order_age_cost",
    "pinned_stops_cost",
    "required_vehicle_cost",
    "request_group_cost",
}


def test_the_catalog_is_the_engine_list_without_the_engine_only_rows() -> None:
    rows = {row.name: row for row in engine.constraint_info()}
    entries = catalog()
    assert [entry["name"] for entry in entries] == [
        name for name in engine.list_constraints() if name not in ENGINE_ONLY
    ]
    for entry in entries:
        assert entry["default_level"] == rows[entry["name"]].default_level
        assert entry["default_weight"] == rows[entry["name"]].default_weight


def _request(*, second: dict | None = None, e_location=(0.0, 0.03), **vehicle) -> PdpRequest:
    """One request, P then D; the van profile gives three legs and leaves every other pair out.
    ``second`` adds a vehicle V2 with those fields."""
    vehicle = {"start_depot": "H"} | vehicle
    vehicles = [VehicleModel(id="V1", capacity=20, departure_time=100, **vehicle)]
    if second is not None:
        vehicles.append(VehicleModel(id="V2", capacity=20, departure_time=100, **second))
    return PdpRequest(
        time_limit_seconds=5,
        model_input=PdpInput(
            depots=[DepotModel(id="H", location=(0.0, 0.0)), DepotModel(id="E", location=e_location)],
            vehicles=vehicles,
            deliveries=[
                DeliveryModel(
                    id="R",
                    demand=3,
                    pickup=StopModel(id="P", location=(0.0, 0.01), min_start_time=300, service_duration=60),
                    dropoff=StopModel(id="D", location=(0.0, 0.02), service_duration=90),
                )
            ],
            profiles={
                "default": TravelProfile(),  # straight lines
                "van": TravelProfile(
                    durations={"H": {"P": 50}, "P": {"D": 70}, "D": {"E": 90}},
                    distances={"H": {"P": 400}, "P": {"D": 600}, "D": {"E": 800}},
                    missing_value=5_000,
                ),
            },
        ),
    )


def _engine_cost(request: PdpRequest, routes: dict[str, tuple[str, ...]], name: str) -> float:
    plan = to_plan(request.model_input)
    index = {node.id: i for i, node in enumerate(plan.nodes)}
    for vehicle in plan.vehicles:
        vehicle.node_indices = [index[node_id] for node_id in routes[vehicle.id]]
    plan.rebuild_unassigned_pool()
    return sum(engine.constraint_costs(plan)[name]["costs"])


def _level(score_text: str, level: str) -> int:
    return int(next(part for part in score_text.split("/") if part.endswith(level))[: -len(level)])


def _soft(score_text: str) -> int:
    return _level(score_text, "soft")


def _engine_hard(request: PdpRequest, routes: dict[str, tuple[str, ...]], name: str) -> int:
    """The engine's hard score of one constraint at weight 1, as the use case prints it."""
    plan = to_plan(request.model_input)
    index = {node.id: i for i, node in enumerate(plan.nodes)}
    for vehicle in plan.vehicles:
        vehicle.node_indices = [index[node_id] for node_id in routes[vehicle.id]]
    plan.rebuild_unassigned_pool()
    spec = next(constraint for constraint in get_constraints() if constraint.name == name)
    return score(engine.constraint_costs(plan), [type(spec)(weight=1)]).hard


def test_an_open_route_stops_at_its_last_stop() -> None:
    """Without an end depot the leg after D is not driven: 50 + 70 in the van profile."""
    request = _request(profile="van")
    routes = {"V1": ("P", "D")}
    assert _engine_cost(request, routes, "travel_time_cost") == pytest.approx(120)
    exact = solve_request(request, constraints=[{"name": "travel_time_cost", "weight": 1}], _fixed_routes=routes)
    assert -_soft(exact.score) == 120


@pytest.mark.parametrize(
    "name,limit,expected",
    [
        # The route ends at E at 610: 510 s after its departure, 10 s after a 600 s end.
        ("max_route_duration_cost", {"max_route_duration": 400}, 110),
        ("vehicle_max_end_time_cost", {"max_end_time": 600}, 10),
    ],
)
def test_the_route_limits_count_the_leg_to_the_end_depot(name, limit, expected) -> None:
    request = _request(end_depot="E", profile="van", **limit)
    routes = {"V1": ("P", "D")}
    assert _engine_cost(request, routes, name) == pytest.approx(expected)
    exact = solve_request(request, constraints=[{"name": name, "weight": 1}], _fixed_routes=routes)
    assert _level(exact.score, "hard") == _engine_hard(request, routes, name) < 0


def test_the_radius_is_measured_from_the_start_depot() -> None:
    """From E (far east) the farthest stop is P; from H it would be D, which is nearer.

    The model charges ceil(furthest) - ceil(limit), the engine furthest - limit in doubles: they agree
    up to the rounding of the meters only because the limit here is a whole number."""
    routes = {"V1": ("P", "D")}
    from_e = _request(start_depot="E", e_location=(0.0, 0.05), max_route_radius_meters=1_000)
    from_h = _request(e_location=(0.0, 0.05), max_route_radius_meters=1_000)
    over_e = _engine_cost(from_e, routes, "max_route_radius_cost")
    assert over_e > _engine_cost(from_h, routes, "max_route_radius_cost") > 0
    exact = solve_request(from_e, constraints=[{"name": "max_route_radius_cost", "weight": 1}], _fixed_routes=routes)
    assert -_level(exact.score, "hard") == math.ceil(over_e)


@pytest.mark.parametrize("carrier", ["V1", "V2"])
def test_each_vehicle_drives_its_own_profile(carrier) -> None:
    """V1 drives the van dicts to E; V2 drives straight lines and ends at H."""
    request = _request(end_depot="E", profile="van", second={"start_depot": "H", "end_depot": "H"})
    routes = {"V1": (), "V2": ()} | {carrier: ("P", "D")}
    cost = _engine_cost(request, routes, "travel_time_cost")
    assert (cost == pytest.approx(210)) == (carrier == "V1")
    exact = solve_request(request, constraints=[{"name": "travel_time_cost", "weight": 1}], _fixed_routes=routes)
    assert -_soft(exact.score) == round(cost)


def test_the_bounds_count_only_drivable_legs() -> None:
    """Start depot to a stop, stop to stop and stop to end depot count; a depot pair or a leg back
    to the start of an open route does not, though the profile leaves it at the missing value."""
    from pickup_delivery_cpsat.model.solve import _longest_legs

    body = _request(profile="van").model_dump()
    body["model_input"]["profiles"]["van"] = {
        "durations": {"H": {"P": 50, "D": 60}, "P": {"D": 70, "E": 80}, "D": {"P": 75, "E": 90}},
        "distances": {"H": {"P": 500, "D": 600}, "P": {"D": 700, "E": 800}, "D": {"P": 750, "E": 900}},
        "missing_value": 5_000,
    }
    stops = range(2, 4)  # the locations are H, E, then P, D
    open_route = to_plan(PdpRequest.model_validate(body).model_input)
    assert _longest_legs(open_route, stops) == (75, 750)  # P -> H, D -> H, H -> E, ... are left out
    body["model_input"]["vehicles"][0]["end_depot"] = "E"
    to_e = to_plan(PdpRequest.model_validate(body).model_input)
    assert _longest_legs(to_e, stops) == (90, 900)
    body["model_input"]["vehicles"][0]["end_depot"] = "H"
    back_home = to_plan(PdpRequest.model_validate(body).model_input)
    assert _longest_legs(back_home, stops) == (5_000, 5_000)  # P -> H is drivable now, and missing

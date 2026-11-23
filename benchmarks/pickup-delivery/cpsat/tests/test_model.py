"""The kept CP-SAT model against the engine: the same catalog and defaults, and the same costs for
pinned routes on the 0.6 input (a travel profile and an end depot other than the start)."""

import pytest

import optimaru.pickup_delivery as engine
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


def _request(**vehicle) -> PdpRequest:
    """One request, P then D; the van profile gives three legs and leaves every other pair out."""
    return PdpRequest(
        time_limit_seconds=5,
        model_input=PdpInput(
            depots=[DepotModel(id="H", location=(0.0, 0.0)), DepotModel(id="E", location=(0.0, 0.03))],
            vehicles=[VehicleModel(id="V1", capacity=20, start_depot="H", departure_time=100, **vehicle)],
            deliveries=[
                DeliveryModel(
                    id="R",
                    demand=3,
                    pickup=StopModel(id="P", location=(0.0, 0.01), min_start_time=300, service_duration=60),
                    dropoff=StopModel(id="D", location=(0.0, 0.02), service_duration=90),
                )
            ],
            profiles={
                "van": TravelProfile(
                    durations={"H": {"P": 50}, "P": {"D": 70}, "D": {"E": 90}},
                    distances={"H": {"P": 400}, "P": {"D": 600}, "D": {"E": 800}},
                    missing_value=5_000,
                )
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


def _soft(score_text: str) -> int:
    return int(next(part for part in score_text.split("/") if part.endswith("soft"))[: -len("soft")])


@pytest.mark.parametrize(
    "name,expected",
    [
        # H -> P 50, P -> D 70, D -> E 90; the pair D -> H is left out but not driven.
        ("travel_time_cost", 210),
        ("travel_distance_cost", 400 + 600 + 800),
        # Departs 100, at P 150 waits to 300, leaves 360, at D 430, leaves 520, at E 610.
        ("route_duration_cost", 510),
    ],
)
def test_a_profile_and_another_end_depot_cost_the_same_on_both(name, expected) -> None:
    request = _request(end_depot="E", profile="van")
    routes = {"V1": ("P", "D")}
    assert _engine_cost(request, routes, name) == pytest.approx(expected)
    exact = solve_request(request, constraints=[{"name": name, "weight": 1}], _fixed_routes=routes)
    assert -_soft(exact.score) == expected


def test_an_open_route_stops_at_its_last_stop() -> None:
    """Without an end depot the leg after D is not driven: 50 + 70 in the van profile."""
    request = _request(profile="van")
    routes = {"V1": ("P", "D")}
    assert _engine_cost(request, routes, "travel_time_cost") == pytest.approx(120)
    exact = solve_request(request, constraints=[{"name": "travel_time_cost", "weight": 1}], _fixed_routes=routes)
    assert -_soft(exact.score) == 120

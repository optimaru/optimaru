"""Solving from a given route, with its first stops pinned."""

import pdp_routing as pdp
import pytest
from pdp_routing.constraints import MinimizeTravelTime, get_constraints
from pdp_routing.models import DeliveryModel, PdpInput, PdpRequest, StopModel, VehicleModel, solve_request


def stop(name: str, service: int = 0) -> StopModel:
    return StopModel(id=name, location=(37.5, 127.0), max_end_time=10**7, service_duration=service)


# Service time is what makes an unassigned request cost anything.
def request(name: str, service: int = 60) -> DeliveryModel:
    return DeliveryModel(id=name, demand=1, pickup=stop(f"{name}p", service), dropoff=stop(f"{name}d", service))


def line_input(vehicles, deliveries, places: dict[str, int]) -> PdpInput:
    """Everything sits on a line; `places` gives each vehicle and stop its position."""
    order = [v.id for v in vehicles]
    for delivery in deliveries:
        order += [delivery.pickup.id, delivery.dropoff.id]
    return PdpInput(
        vehicles=vehicles,
        deliveries=deliveries,
        driving_seconds=[[abs(places[a] - places[b]) for b in order] for a in order],
    )


def solve(model_input: PdpInput) -> dict[str, list[str]]:
    solution = solve_request(PdpRequest(model_input=model_input, timelimit=0.3))
    assert solution.feasible
    return {vehicle: [s.stop_id for s in route] for vehicle, route in solution.routes.items()}


def vehicle(name: str, route=(), pinned: int = 0) -> VehicleModel:
    return VehicleModel(id=name, capacity=5, home=(37.5, 127.0), initial_route=list(route), pinned_stops=pinned)


NEARBY = {"v": 0, "ap": 100, "ad": 1000, "bp": 110, "bd": 120}


def test_a_new_request_slips_in_after_the_pinned_stop_and_before_a_free_one():
    model_input = line_input([vehicle("v", ["ap", "ad"], pinned=1)], [request("a"), request("b")], NEARBY)

    assert solve(model_input)["v"] == ["ap", "bp", "bd", "ad"]


def test_pinning_both_stops_keeps_new_work_behind_them():
    model_input = line_input([vehicle("v", ["ap", "ad"], pinned=2)], [request("a"), request("b")], NEARBY)

    assert solve(model_input)["v"][:2] == ["ap", "ad"]


def test_a_pinned_stop_stays_first_when_something_else_would_be_better():
    places = {"v": 0, "ap": 5000, "ad": 5001, "bp": 1, "bd": 2}
    model_input = line_input([vehicle("v", ["ap", "ad"], pinned=1)], [request("a"), request("b")], places)

    assert solve(model_input)["v"][0] == "ap"


def test_a_free_tail_can_be_reordered():
    places = {"v": 0, "ap": 1, "ad": 1000, "bp": 2, "bd": 3}
    route = ["ap", "ad", "bp", "bd"]
    model_input = line_input([vehicle("v", route, pinned=1)], [request("a"), request("b")], places)

    assert solve(model_input)["v"] == ["ap", "bp", "bd", "ad"]


def test_an_unpinned_request_can_move_to_another_vehicle():
    places = {"v": 0, "w": 1000, "ap": 1, "ad": 2, "bp": 1001, "bd": 1002}
    route = ["ap", "ad", "bp", "bd"]
    model_input = line_input([vehicle("v", route, pinned=1), vehicle("w")], [request("a"), request("b")], places)

    routes = solve(model_input)

    assert routes["v"] == ["ap", "ad"]
    assert routes["w"] == ["bp", "bd"]


def test_a_pinned_pickup_keeps_its_dropoff_on_the_vehicle():
    places = {"v": 0, "w": 900, "ap": 0, "ad": 900, "bp": 1, "bd": 2}
    model_input = line_input([vehicle("v", ["ap", "ad"], pinned=1), vehicle("w")], [request("a")], places | {"bp": 1})

    assert solve(model_input)["v"] == ["ap", "ad"]


def test_an_initial_route_is_where_the_solve_starts():
    """With no time to search, the given route comes straight back."""
    model_input = line_input([vehicle("v", ["bp", "bd", "ap", "ad"])], [request("a"), request("b")], NEARBY)
    request_model = PdpRequest(
        model_input=model_input, timelimit=0, solver_parameters={"step_count_limit": 1, "ruin_recreate": False}
    )

    solution = solve_request(request_model)

    assert {s.stop_id for s in solution.routes["v"]} == {"ap", "ad", "bp", "bd"}
    assert solution.unassigned == []


def test_a_request_outside_the_initial_routes_is_still_placed():
    model_input = line_input([vehicle("v", ["ap", "ad"])], [request("a"), request("b")], NEARBY)

    assert sorted(solve(model_input)["v"]) == ["ad", "ap", "bd", "bp"]


def test_pinned_stops_score_as_a_hard_violation():
    model_input = line_input([vehicle("v", ["ap", "ad"], pinned=2)], [request("a")], NEARBY)
    plan = pdp.models.to_plan(model_input)
    plan.vehicles[0].node_indices = [1, 0]

    assert pdp.constraint_costs(plan)["pinned_stops"]["costs"][0] == 2


def test_constraints_can_be_replaced_with_pins_in_place():
    model_input = line_input([vehicle("v", ["ap", "ad"], pinned=1)], [request("a"), request("b")], NEARBY)
    kept = [c for c in get_constraints() if c.name != "minimize_travel_time"]

    solution = solve_request(
        PdpRequest(model_input=model_input, timelimit=0.2), constraints=[*kept, MinimizeTravelTime(weight=2)]
    )

    assert solution.routes["v"][0].stop_id == "ap"


def bound(name: str, vehicle_id: str) -> DeliveryModel:
    return request(name).model_copy(update={"required_vehicle": vehicle_id})


def test_a_required_request_can_change_places_on_its_route():
    places = {"v": 0, "ap": 1, "ad": 1000, "bp": 2, "bd": 3}
    route = ["ap", "ad", "bp", "bd"]
    model_input = line_input([vehicle("v", route, pinned=1)], [bound("a", "v"), bound("b", "v")], places)

    assert solve(model_input)["v"] == ["ap", "bp", "bd", "ad"]


def test_a_required_request_stays_when_another_vehicle_would_be_better():
    places = {"v": 0, "w": 1000, "ap": 1, "ad": 2, "bp": 1001, "bd": 1002}
    route = ["ap", "ad", "bp", "bd"]
    model_input = line_input([vehicle("v", route, pinned=1), vehicle("w")], [request("a"), bound("b", "v")], places)

    routes = solve(model_input)

    assert routes["w"] == []
    assert sorted(routes["v"]) == ["ad", "ap", "bd", "bp"]


def test_a_required_request_counts_as_a_hard_violation_off_its_vehicle():
    places = {"v": 0, "w": 0, "ap": 1, "ad": 2}
    model_input = line_input([vehicle("v"), vehicle("w", ["ap", "ad"])], [bound("a", "v")], places)

    assert sum(pdp.constraint_costs(pdp.models.to_plan(model_input))["required_vehicle"]["costs"]) == 1


def test_an_unknown_required_vehicle_is_refused():
    with pytest.raises(ValueError, match="unknown vehicle"):
        line_input([vehicle("v")], [bound("a", "w")], {"v": 0, "ap": 1, "ad": 2})


@pytest.mark.parametrize(
    "route, pinned, message",
    [
        (["ap", "ad"], 3, "more pinned_stops"),
        (["ap", "nope"], 0, "unknown stop"),
        (["ap"], 0, "needs its pickup"),
        (["ad", "ap"], 0, "needs its pickup"),
        (["ap", "ap"], 0, "appears twice"),
    ],
)
def test_a_bad_initial_route_is_refused(route, pinned, message):
    with pytest.raises(ValueError, match=message):
        line_input([vehicle("v", route, pinned=pinned)], [request("a")], {"v": 0, "ap": 1, "ad": 2})


def test_a_stop_cannot_start_on_two_vehicles():
    places = {"v": 0, "w": 0, "ap": 1, "ad": 2}
    with pytest.raises(ValueError, match="appears twice"):
        line_input([vehicle("v", ["ap", "ad"]), vehicle("w", ["ap", "ad"])], [request("a")], places)

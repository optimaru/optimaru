"""A parameter that acts only through a cost needs that cost among the constraints of the request."""

import pytest

from optimaru.vehicle_routing.constraints import (
    MaxEndTimeCost,
    MaxStopsCost,
    TravelTimeCost,
    UnassignedVisitsCost,
    get_constraints,
)
from optimaru.vehicle_routing.models import (
    DepotModel,
    RoutePlanRequest,
    RoutingInput,
    TravelProfile,
    VehicleModel,
    VisitModel,
    parameter_uses,
    solve_request,
)

from .._picks import without

VEHICLE = {
    "max_travel_time": (3600, "max_travel_time_cost"),
    "max_route_duration": (3600, "max_route_duration_cost"),
    "max_end_time": (20 * 3600, "vehicle_max_end_time_cost"),
    "max_stops": (5, "max_stops_cost"),
    "min_load_ratio": (0.5, "min_capacity_utilization_cost"),
    "fixed_cost": (100.0, "vehicle_fixed_cost"),
    "max_route_radius_meters": (5000.0, "max_route_radius_cost"),
}
VISIT = {
    "max_end_time": (12 * 3600, "max_end_time_cost"),
    "unassigned_penalty_minutes": (30, "unassigned_visits_cost"),
    "group": ("g", "visit_group_cost"),
}


def routing(vehicle=None, visit=None, profiles=None) -> RoutingInput:
    return RoutingInput(
        depots=[DepotModel(id="D", location=(37.5, 127.0))],
        vehicles=[VehicleModel(id="V", capacity=10, start_depot="D", **(vehicle or {}))],
        visits=[
            VisitModel(id="A", location=(37.51, 127.0), **(visit or {})),
            VisitModel(
                id="B", location=(37.52, 127.0), **({"group": visit["group"]} if visit and "group" in visit else {})
            ),
        ],
        profiles=profiles or {},
    )


def solve(model_input, constraints=None):
    return solve_request(RoutePlanRequest(model_input=model_input, time_limit_seconds=0), constraints=constraints)


def test_the_defaults_take_every_parameter():
    vehicle = {field: value for field, (value, _) in VEHICLE.items()}
    visit = {field: value for field, (value, _) in VISIT.items()}
    model_input = routing(vehicle, visit)
    assert len(parameter_uses(model_input)) == len(VEHICLE) + len(VISIT) + 1  # B shares A's group
    solve(model_input)
    solve(model_input, get_constraints())


@pytest.mark.parametrize("field", sorted(VEHICLE))
def test_a_vehicle_limit_without_its_cost_is_refused(field):
    value, cost = VEHICLE[field]
    with pytest.raises(ValueError, match=f"vehicle V sets {field}, but the constraints leave out {cost}"):
        solve(routing({field: value}), without(get_constraints(), cost))


@pytest.mark.parametrize("field", sorted(VISIT))
def test_a_visit_field_without_its_cost_is_refused(field):
    value, cost = VISIT[field]
    with pytest.raises(ValueError, match=f"visit A .*sets {field}, but the constraints leave out {cost}"):
        solve(routing(visit={field: value}), without(get_constraints(), cost))


def test_demand_needs_no_cost():
    # The solution reports each vehicle's load from it, whatever the constraints.
    solve(routing(visit={"demand": 3}), [UnassignedVisitsCost(), TravelTimeCost(), MaxEndTimeCost()])


def test_the_tables_hold_the_model_defaults():
    from optimaru.vehicle_routing import models

    for table, model in ((models._VEHICLE_COSTS, VehicleModel), (models._VISIT_COSTS, VisitModel)):
        for field, default, _ in table:
            info = model.model_fields[field]
            assert (info.default_factory() if info.default_factory else info.default) == default, field


def test_after_needs_the_order_cost():
    model_input = RoutingInput(
        depots=[DepotModel(id="D", location=(37.5, 127.0))],
        vehicles=[VehicleModel(id="V", capacity=10, start_depot="D")],
        visits=[
            VisitModel(id="A", location=(37.51, 127.0), group="g"),
            VisitModel(id="B", location=(37.52, 127.0), group="g", after=["A"]),
        ],
    )
    with pytest.raises(ValueError, match="visit B sets after, but the constraints leave out visit_order_cost"):
        solve(model_input, without(get_constraints(), "visit_order_cost"))


def test_a_weight_zero_pick_counts_and_an_unset_parameter_needs_nothing():
    picks = without(get_constraints(), "max_stops_cost")
    solve(routing(), picks)
    solve(routing({"max_stops": 5}), [*picks, MaxStopsCost(weight=0)])


def test_distances_and_forbidden_visits_need_no_listed_cost():
    profiles = {"default": TravelProfile(distances={"D": {"A": 10}}, forbidden_visits=["B"])}
    picks = without(without(get_constraints(), "travel_distance_cost"), "profile_forbidden_visit_cost")
    solve(routing(profiles=profiles), picks)


def test_a_generator_of_picks_is_read_once():
    solve(routing({"max_stops": 5}), (pick for pick in get_constraints()))


def test_a_misspelt_pick_is_reported_as_one():
    class Pick:
        name = "max_stop_cost"

    with pytest.raises(ValueError, match="max_stop_cost"):
        solve(routing({"max_stops": 5}), [*without(get_constraints(), "max_stops_cost"), Pick()])

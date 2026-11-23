"""A parameter that acts only through a cost needs that cost among the constraints of the request."""

import pytest

from optimaru.pickup_delivery.constraints import MaxStopsCost, get_constraints
from optimaru.pickup_delivery.models import (
    DeliveryModel,
    DepotModel,
    PdpInput,
    PdpRequest,
    StopModel,
    VehicleModel,
    parameter_uses,
    solve_request,
)

from .._picks import without

VEHICLE = {
    "max_travel_time": (3600, "max_travel_time_cost"),
    "max_route_duration": (3600, "max_route_duration_cost"),
    "max_end_time": (20 * 3600, "vehicle_max_end_time_cost"),
    "max_stops": (6, "max_stops_cost"),
    "min_load_ratio": (0.5, "min_capacity_utilization_cost"),
    "fixed_cost": (100.0, "vehicle_fixed_cost"),
    "max_route_radius_meters": (5000.0, "max_route_radius_cost"),
}
REQUEST = {
    "order_time": (-600, "squared_order_age_cost"),
    "required_vehicle": ("V", "required_vehicle_cost"),
    "group": ("g", "request_group_cost"),
}


def stop(name, **fields):
    return StopModel(id=name, location=(37.5, 127.0), **fields)


def pdp_input(vehicle=None, request=None, dropoff=None) -> PdpInput:
    return PdpInput(
        depots=[DepotModel(id="D", location=(37.5, 127.0))],
        vehicles=[VehicleModel(id="V", capacity=10, start_depot="D", **(vehicle or {}))],
        deliveries=[
            DeliveryModel(id="R", demand=1, pickup=stop("P"), dropoff=stop("Q", **(dropoff or {})), **(request or {}))
        ],
    )


def solve(model_input, constraints=None):
    return solve_request(PdpRequest(model_input=model_input, time_limit_seconds=0), constraints=constraints)


def test_the_defaults_take_every_parameter():
    model_input = pdp_input(
        {field: value for field, (value, _) in VEHICLE.items()},
        {field: value for field, (value, _) in REQUEST.items()},
        {"max_end_time": 12 * 3600},
    )
    assert len(parameter_uses(model_input)) == len(VEHICLE) + len(REQUEST) + 1
    solve(model_input)


@pytest.mark.parametrize("field", sorted(VEHICLE))
def test_a_vehicle_limit_without_its_cost_is_refused(field):
    value, cost = VEHICLE[field]
    with pytest.raises(ValueError, match=f"vehicle V sets {field}, but the constraints leave out {cost}"):
        solve(pdp_input({field: value}), without(get_constraints(), cost))


@pytest.mark.parametrize("field", sorted(REQUEST))
def test_a_request_field_without_its_cost_is_refused(field):
    value, cost = REQUEST[field]
    with pytest.raises(ValueError, match=f"request R sets {field}, but the constraints leave out {cost}"):
        solve(pdp_input(request={field: value}), without(get_constraints(), cost))


def test_a_stop_window_without_its_cost_is_refused():
    with pytest.raises(ValueError, match="stop Q sets max_end_time, but the constraints leave out max_end_time_cost"):
        solve(pdp_input(dropoff={"max_end_time": 3600}), without(get_constraints(), "max_end_time_cost"))


def test_max_stops_at_its_json_default_needs_nothing_and_weight_zero_counts():
    picks = without(get_constraints(), "max_stops_cost")
    solve(pdp_input(), picks)  # max_stops 20, the default
    solve(pdp_input({"max_stops": 6}), [*picks, MaxStopsCost(weight=0)])


def test_the_tables_hold_the_model_defaults():
    from optimaru.pickup_delivery import models

    for table, model in ((models._VEHICLE_COSTS, VehicleModel), (models._REQUEST_COSTS, DeliveryModel)):
        for field, default, _ in table:
            info = model.model_fields[field]
            assert (info.default_factory() if info.default_factory else info.default) == default, field
    assert StopModel.model_fields["max_end_time"].default == 24 * 3600

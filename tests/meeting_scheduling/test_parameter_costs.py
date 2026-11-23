"""Room and meeting parameters that only costs read are refused when the constraints leave those costs out."""

import pytest

import optimaru.meeting_scheduling as ms
from optimaru.meeting_scheduling.constraints import (
    LargerRoomCapacityCost,
    PreferredAttendanceConflictCost,
    RequiredAttendanceConflictCost,
    RoomConflictCost,
)
from optimaru.meeting_scheduling.models import MeetingScheduleRequest, generate_demo_data, parameter_uses, solve_request


def _bare():
    """The demo instance with every room at capacity 0 and every meeting without attendees."""
    model = generate_demo_data()
    for room in model.rooms:
        room.capacity = 0
    for meeting in model.meetings:
        meeting.required_attendees = []
        meeting.preferred_attendees = []
    return model


def test_parameter_uses_lists_capacities_and_attendees():
    assert parameter_uses(_bare()) == []
    fields = {use.field for use in parameter_uses(generate_demo_data())}
    assert fields == {"capacity", "required_attendees", "preferred_attendees"}


def test_the_default_constraints_take_every_parameter():
    ms.solve(generate_demo_data(), time_limit_seconds=0, random_seed=1)


@pytest.mark.parametrize(
    ("set_it", "field", "cost"),
    [
        (lambda m: setattr(m.rooms[0], "capacity", 10), "capacity", "room_capacity_cost or larger_room_capacity_cost"),
        (lambda m: setattr(m.meetings[0], "required_attendees", [0]), "required_attendees", "room_capacity_cost"),
        (lambda m: setattr(m.meetings[0], "preferred_attendees", [0]), "preferred_attendees", "room_capacity_cost"),
    ],
)
def test_a_selection_without_the_costs_is_refused(set_it, field, cost):
    model = _bare()
    set_it(model)
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        ms.solve(model, time_limit_seconds=0, constraints=[RoomConflictCost()])
    with pytest.raises(ValueError, match=f"sets {field}"):
        solve_request(MeetingScheduleRequest(model_input=model, time_limit_seconds=0), constraints=[RoomConflictCost()])


def test_one_of_the_costs_is_enough_and_a_weight_of_zero_counts():
    picks = [
        RoomConflictCost(),
        LargerRoomCapacityCost(weight=0),
        RequiredAttendanceConflictCost(),
        PreferredAttendanceConflictCost(),
    ]
    ms.solve(generate_demo_data(), time_limit_seconds=0, constraints=picks)


def test_parameters_left_at_their_defaults_need_no_cost():
    ms.solve(_bare(), time_limit_seconds=0, constraints=[RoomConflictCost()])

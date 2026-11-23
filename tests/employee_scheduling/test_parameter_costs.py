"""A parameter that acts only through a cost needs that cost among the request's constraints."""

import pytest

import optimaru.employee_scheduling as es
from optimaru.employee_scheduling.constraints import (
    DesiredDayCost,
    DesiredShiftCost,
    EmployeeUnavailabilityCost,
    RequiredSkillCost,
    ShiftOverlapCost,
    UndesiredDayCost,
)
from optimaru.employee_scheduling.models import (
    ScheduleRequest,
    SchedulingInput,
    ShiftModel,
    WorkerModel,
    default_constraints,
    parameter_uses,
    solve_request,
)


def scheduling(skill: str = "", constraints=None, **worker) -> SchedulingInput:
    workers = [WorkerModel(id="W1", skills=["A"], **worker), WorkerModel(id="W2", skills=["A"])]
    shifts = [
        ShiftModel(id=f"S{d}", start=es.minutes(d, 9), end=es.minutes(d, 17), location="L", required_skill=skill)
        for d in range(3)
    ]
    extra = {} if constraints is None else {"constraints": constraints}
    return SchedulingInput(workers=workers, shifts=shifts, **extra)


def request(model_input) -> ScheduleRequest:
    return ScheduleRequest(model_input=model_input, time_limit_seconds=0.05)


DEFAULT_ONES = {
    "unavailable_dates": [1],
    "undesired_dates": [2],
    "desired_dates": [0],
}


def test_the_default_constraints_take_every_default_parameter():
    model_input = scheduling("A", **DEFAULT_ONES)
    assert {use.field for use in parameter_uses(model_input)} == {*DEFAULT_ONES, "required_skill"}
    solve_request(request(model_input))


def test_desired_shifts_need_their_cost_which_is_off_by_default():
    model_input = scheduling(desired_shifts=[(0, "L")])
    with pytest.raises(ValueError, match="sets desired_shifts, but the constraints leave out desired_shift_cost"):
        solve_request(request(model_input))
    solve_request(request(model_input), constraints=[*default_constraints(), DesiredShiftCost()])
    solve_request(
        request(scheduling(desired_shifts=[(0, "L")], constraints=[*default_constraints(), DesiredShiftCost()]))
    )


@pytest.mark.parametrize(
    ("fields", "skill", "field", "cost"),
    [
        ({}, "A", "required_skill", "required_skill_cost"),
        ({"unavailable_dates": [1]}, "", "unavailable_dates", "employee_unavailability_cost"),
        ({"undesired_dates": [1]}, "", "undesired_dates", "undesired_day_cost"),
        ({"desired_dates": [1]}, "", "desired_dates", "desired_day_cost"),
    ],
)
def test_a_selection_without_the_cost_is_refused(fields, skill, field, cost):
    model_input = scheduling(skill, **fields)
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        solve_request(request(model_input), constraints=[ShiftOverlapCost()])
    # The selection inside the request is the one checked when none is passed.
    with pytest.raises(ValueError, match=cost):
        solve_request(request(scheduling(skill, constraints=[ShiftOverlapCost()], **fields)))


def test_a_pick_at_weight_zero_counts():
    picks = [
        ShiftOverlapCost(),
        RequiredSkillCost(weight=0),
        EmployeeUnavailabilityCost(weight=0),
        UndesiredDayCost(weight=0),
        DesiredDayCost(weight=0),
        DesiredShiftCost(weight=0),
    ]
    solve_request(request(scheduling("A", desired_shifts=[(0, "L")], **DEFAULT_ONES)), constraints=picks)


def test_parameters_left_unset_need_no_cost():
    model_input = scheduling()
    assert parameter_uses(model_input) == []
    solve_request(request(model_input), constraints=iter([ShiftOverlapCost()]))

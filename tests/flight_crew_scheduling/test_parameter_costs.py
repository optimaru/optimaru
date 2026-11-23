"""A parameter that acts only through a cost needs that cost among the constraints."""

from datetime import date, datetime, timedelta

import pytest

import optimaru.flight_crew_scheduling as fc
from optimaru.flight_crew_scheduling.constraints import (
    EmployeeUnavailabilityCost,
    FlightConflictCost,
    RequiredSkillCost,
)
from optimaru.flight_crew_scheduling.models import (
    AirportModel,
    EmployeeModel,
    FlightAssignmentModel,
    FlightCrewScheduleModel,
    FlightCrewScheduleRequest,
    FlightModel,
    parameter_uses,
    solve_request,
)

DAY0 = datetime(2026, 8, 10)


def schedule(skill: str = "", unavailable=()) -> FlightCrewScheduleModel:
    flights = [
        FlightModel(
            flight_number=f"F{day}",
            departure_airport="ICN",
            departure_utc_date_time=DAY0 + timedelta(days=day, hours=8),
            arrival_airport="NRT",
            arrival_utc_date_time=DAY0 + timedelta(days=day, hours=10),
        )
        for day in range(2)
    ]
    return FlightCrewScheduleModel(
        airports=[AirportModel(code="ICN"), AirportModel(code="NRT")],
        employees=[
            EmployeeModel(id="P0", home_airport="ICN", skills=["Pilot"], unavailable_days=list(unavailable)),
            EmployeeModel(id="P1", home_airport="ICN", skills=["Pilot"]),
        ],
        flights=flights,
        flight_assignments=[
            FlightAssignmentModel(id=f"{f.flight_number}-0", flight=f.flight_number, required_skill=skill)
            for f in flights
        ],
    )


def solve(model, constraints=None):
    return fc.solve(model, time_limit_seconds=0.05, random_seed=1, constraints=constraints)


def test_the_default_constraints_take_every_parameter():
    model = schedule("Pilot", [date(2026, 8, 10)])
    assert {use.field for use in parameter_uses(model)} == {"required_skill", "unavailable_days"}
    solve(model)
    solve(model.model_dump(mode="json"))  # a dict is checked too
    solve_request(FlightCrewScheduleRequest(model_input=model, time_limit_seconds=0.05))


@pytest.mark.parametrize(
    ("model", "field", "cost"),
    [
        (schedule("Pilot"), "required_skill", "required_skill_cost"),
        (schedule(unavailable=[date(2026, 8, 10)]), "unavailable_days", "employee_unavailability_cost"),
    ],
)
def test_a_selection_without_the_cost_is_refused(model, field, cost):
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        solve(model, [FlightConflictCost()])
    with pytest.raises(ValueError, match=cost):
        solve_request(
            FlightCrewScheduleRequest(model_input=model, time_limit_seconds=0.05), constraints=[FlightConflictCost()]
        )


def test_a_pick_at_weight_zero_counts():
    picks = [FlightConflictCost(), RequiredSkillCost(weight=0), EmployeeUnavailabilityCost(weight=0)]
    solve(schedule("Pilot", [date(2026, 8, 10)]), picks)


def test_parameters_left_unset_need_no_cost():
    model = schedule()
    assert parameter_uses(model) == []
    solve(model, [FlightConflictCost()])
    solve(model, iter([FlightConflictCost()]))


def test_an_engine_object_is_not_checked():
    solve(schedule("Pilot").to_core(), [FlightConflictCost()])
